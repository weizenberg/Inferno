/*
 * Inferno guest-to-host Metal execution bridge.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "InfernoMetalService.h"
#include "InfernoMetalUserClient.h"
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/libkern.h>

OSDefineMetaClassAndStructors(InfernoMetalService, IOService);
OSDefineMetaClassAndStructors(InfernoMetalSession, OSObject);

IOReturn InfernoMetalService::newUserClient(task_t owningTask, void *securityID,
                                            UInt32 type,
                                            OSDictionary *properties,
                                            IOUserClient **handler)
{
    if (!handler) {
        return kIOReturnBadArgument;
    }
    *handler = nullptr;
    if (type != INFERNO_METAL_USER_CONNECTION_TYPE) {
        return kIOReturnUnsupported;
    }

    auto *client = new InfernoMetalUserClient;
    if (!client) {
        return kIOReturnNoMemory;
    }
    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnBadArgument;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnUnsupported;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnUnsupported;
    }
    *handler = client;
    return kIOReturnSuccess;
}

template <typename T> static T *takeFactory(OSPtr<T> value)
{
#if defined(IOKIT_ENABLE_SHARED_PTR)
    return value.detach();
#else
    return value;
#endif
}

static IOReturn ioResult(ImtlResult result)
{
    switch (result) {
    case IMTL_OK:
        return kIOReturnSuccess;
    case IMTL_AGAIN:
        return kIOReturnNotReady;
    case IMTL_BUSY:
        return kIOReturnBusy;
    case IMTL_BAD_ARGUMENT:
        return kIOReturnBadArgument;
    default:
        return kIOReturnIOError;
    }
}

bool InfernoMetalService::start(IOService *provider)
{
    if (!IOService::start(provider)) {
        return false;
    }
    loop_ = takeFactory(IOWorkLoop::workLoop());
    gate_ = takeFactory(IOCommandGate::commandGate(this));
    timer_ =
        takeFactory(IOTimerEventSource::timerEventSource(this, timerAction));
    if (!loop_ || !gate_ || !timer_) {
        goto fail;
    }
    if (loop_->addEventSource(gate_) != kIOReturnSuccess) {
        goto fail;
    }
    gate_added_ = true;
    if (loop_->addEventSource(timer_) != kIOReturnSuccess) {
        goto fail;
    }
    timer_added_ = true;
    if (!setProperty("InfernoMetalProtocolVersion", INFERNO_METAL_VERSION,
                     32) ||
        !setProperty("InfernoMetalMaxSourceBytes", INFERNO_METAL_MAX_SOURCE,
                     32) ||
        !setProperty("InfernoMetalMaxBufferBytes", INFERNO_METAL_MAX_BUFFER,
                     32)) {
        goto fail;
    }
    if (!provider->open(this)) {
        goto fail;
    }
    provider_ = provider;
    provider_open_ = true;
    /* Last fallible start operation. No command or timer is active yet. */
    if (imtl_iokit_create(provider, &owner_) != kIOReturnSuccess) {
        provider_open_ = false;
        provider_->close(this);
        provider_ = nullptr;
        goto fail;
    }
    status_ = { Idle, IMTL_OK, kIOReturnSuccess, { 0, 0 } };
    registerService();
    return true;

fail:
    removeSources();
    IOService::stop(provider);
    return false;
}

IOWorkLoop *InfernoMetalService::getWorkLoop() const
{
    return loop_;
}

void InfernoMetalService::removeSources()
{
    if (timer_) {
        timer_->cancelTimeout();
        timer_->disable();
    }
    if (timer_added_) {
        loop_->removeEventSource(timer_);
        timer_added_ = false;
    }
    if (gate_added_) {
        loop_->removeEventSource(gate_);
        gate_added_ = false;
    }
    /* Keep factory references until free(): a retained kernel caller racing
     * stop can safely receive runAction's detached-gate error.
     */
}

void InfernoMetalService::stop(IOService *provider)
{
    /* IOKit invokes stop on the provider's workloop, not necessarily ours. */
    if (!loop_ || loop_->runAction(stopAction, this) != kIOReturnSuccess) {
        panic("InfernoMetalService cannot serialize stop");
    }
    IOService::stop(provider);
}

IOReturn InfernoMetalService::stopAction(OSObject *object, void *, void *,
                                         void *, void *)
{
    auto *service = static_cast<InfernoMetalService *>(object);
    /* Public stop has no asynchronous deferral. Both termination paths below
     * arrange complete DMA retirement before the framework can reach here.
     */
    if (service->owner_ || service->provider_open_ ||
        service->active_session_) {
        panic("InfernoMetalService stop before DMA retirement");
    }
    service->status_.state = Stopped;
    service->removeSources();
    service->provider_ = nullptr;
    return kIOReturnSuccess;
}

void InfernoMetalService::free()
{
    if (owner_ || provider_open_ || gate_added_ || timer_added_ ||
        active_session_) {
        panic("InfernoMetalService free with live resources");
    }
    if (timer_) {
        timer_->release();
    }
    if (gate_) {
        gate_->release();
    }
    if (loop_) {
        loop_->release();
    }
    IOService::free();
}

IOReturn InfernoMetalService::dispatch(Request *request)
{
    if (!gate_) {
        return kIOReturnNotReady;
    }
    return gate_->runAction(gateAction, request);
}

IOReturn InfernoMetalService::gateAction(OSObject *object, void *request,
                                         void *, void *, void *)
{
    auto *service = static_cast<InfernoMetalService *>(object);
    IOReturn result = service->dispatchGated(static_cast<Request *>(request));
    service->syncSessionGated();
    return result;
}

void InfernoMetalService::timerAction(OSObject *object, IOTimerEventSource *)
{
    /* IOTimerEventSource already holds the workloop gate for its action. */
    auto *service = static_cast<InfernoMetalService *>(object);
    service->refreshGated();
    service->syncSessionGated();
}

void InfernoMetalService::syncSessionGated()
{
    if (!active_session_) {
        return;
    }
    active_session_->status_ = status_;
    if (status_.state == Idle || status_.state == Drained) {
        auto *session = active_session_;
        active_session_ = nullptr;
        session->release();
    }
}

void InfernoMetalService::armGated()
{
    status_.timer_error = timer_->setTimeoutMS(1);
    /* Failure is observable through getStatus. Keep provider/owner alive;
     * requestShutdown/getStatus can make another gated retirement attempt.
     */
}

IOReturn InfernoMetalService::shutdownGated()
{
    if (status_.state == Drained) {
        return kIOReturnSuccess;
    }
    if (status_.state == Stopped) {
        return kIOReturnNotReady;
    }
    status_.state = Draining;
    status_.transport_result = imtl_iokit_destroy(&owner_);
    if (status_.transport_result == IMTL_AGAIN) {
        armGated();
        return status_.timer_error == kIOReturnSuccess ? kIOReturnNotReady :
                                                         status_.timer_error;
    }
    if (status_.transport_result != IMTL_OK) {
        return ioResult(status_.transport_result);
    }
    status_.state = Drained;
    status_.completion = { 0, 0 };
    if (provider_open_) {
        provider_open_ = false;
        /* Provider removal queues stop after this gated action returns. */
        provider_->close(this);
    }
    return kIOReturnSuccess;
}

void InfernoMetalService::refreshGated()
{
    if (status_.state == Draining) {
        shutdownGated();
        return;
    }
    if (active_session_ && active_session_->closing_ &&
        status_.state == Faulted) {
        /* A later caller can retry a failed disconnected-session reset even
         * when the timer could not be armed. Ownership remains unavailable.
         */
        Request reset = {};
        reset.operation = Reset;
        reset.session_action = true;
        dispatchGated(&reset);
        return;
    }
    if (status_.state != Submitted && status_.state != Resetting) {
        return;
    }
    bool resetting = status_.state == Resetting;
    status_.transport_result =
        imtl_iokit_poll(owner_, resetting ? nullptr : &status_.completion);
    if (status_.transport_result == IMTL_AGAIN) {
        armGated();
    } else if (status_.transport_result == IMTL_OK) {
        status_.state = resetting ? Idle : Completed;
    } else {
        /* Retain the failed request for explicit reset/shutdown recovery. */
        status_.state = Faulted;
    }
}

IOReturn InfernoMetalService::dispatchGated(Request *r)
{
    if (r->session) {
        return dispatchSessionGated(r);
    }
    switch (r->operation) {
    case NewSession: {
        if (!r->new_session) {
            return kIOReturnBadArgument;
        }
        *r->new_session = nullptr;
        if (isInactive() || status_.state == Draining ||
            status_.state == Drained || status_.state == Stopped) {
            return kIOReturnNotReady;
        }
        auto *session = new InfernoMetalSession;
        if (!session) {
            return kIOReturnNoMemory;
        }
        if (!session->init()) {
            session->release();
            return kIOReturnNoMemory;
        }
        session->service_ = this;
        *r->new_session = session;
        return kIOReturnSuccess;
    }
    case Query:
        if (!r->status) {
            return kIOReturnBadArgument;
        }
        refreshGated();
        *r->status = status_;
        return kIOReturnSuccess;
    case Shutdown:
        return shutdownGated();
    case CanTerminate:
        return status_.state == Drained && !owner_ && !provider_open_ ?
                   kIOReturnSuccess :
                   kIOReturnBusy;
    case ProviderTerminate: {
        if (status_.state == Stopped) {
            return kIOReturnNotReady;
        }
        /* The public base method synchronously calls the provider's
         * terminateClient(), which nests our virtual terminate(). Keep this
         * context under the gate so an unrelated caller cannot inherit it.
         */
        bool previous = provider_recursing_;
        provider_recursing_ = true;
        bool result = IOService::requestTerminate(r->provider, r->options);
        provider_recursing_ = previous;
        return result ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    default:
        break;
    }
    if (status_.state == Draining || status_.state == Drained ||
        status_.state == Stopped || isInactive()) {
        return kIOReturnNotReady;
    }
    if (active_session_ && !r->session_action &&
        (r->operation == Read || r->operation == Acknowledge ||
         r->operation == Reset)) {
        return kIOReturnBusy;
    }
    switch (r->operation) {
    case Submit: {
        if (status_.state != Idle) {
            return kIOReturnBusy;
        }
        IOReturn result =
            imtl_iokit_submit(owner_, r->command, r->source, r->input, false);
        if (result != kIOReturnSuccess) {
            return result;
        }
        status_ = { Submitted, IMTL_AGAIN, kIOReturnSuccess, { 0, 0 } };
        armGated();
        /* Submission succeeded even if timer arming failed. Status reports
         * that distinct failure; never imply the doorbell was not written.
         */
        return kIOReturnSuccess;
    }
    case Read:
        if (status_.state == Faulted) {
            return kIOReturnIOError;
        }
        if (status_.state != Completed) {
            return kIOReturnNotReady;
        }
        return ioResult(
            imtl_iokit_read_output(owner_, r->offset, r->output, r->size));
    case Acknowledge: {
        ImtlResult result = imtl_iokit_ack(owner_);
        if (result == IMTL_OK) {
            status_ = { Idle, IMTL_OK, kIOReturnSuccess, { 0, 0 } };
        }
        /* A rejected ACK must not replace the original polling fault. */
        return ioResult(result);
    }
    case Reset:
        status_.completion = { 0, 0 };
        status_.transport_result = imtl_iokit_reset(owner_);
        if (status_.transport_result == IMTL_OK) {
            status_.state = Idle;
        } else if (status_.transport_result == IMTL_AGAIN) {
            status_.state = Resetting;
            armGated();
        } else {
            status_.state = Faulted;
        }
        return ioResult(status_.transport_result);
    default:
        return kIOReturnBadArgument;
    }
}

IOReturn InfernoMetalService::dispatchSessionGated(Request *r)
{
    auto *session = r->session;
    if (session->service_ != this) {
        return kIOReturnBadArgument;
    }
    if (r->operation == CloseSession) {
        session->closing_ = true;
        if (active_session_ != session) {
            return kIOReturnSuccess;
        }
        Request reset = {};
        reset.operation = Reset;
        reset.session_action = true;
        return dispatchGated(&reset);
    }
    if (session->closing_ || isInactive() || status_.state == Draining ||
        status_.state == Drained || status_.state == Stopped) {
        return kIOReturnNotReady;
    }
    /* Retire an abandoned predecessor even after a timer-arm failure.
     * Keep each session's terminal status separate from the current command.
     */
    refreshGated();
    syncSessionGated();
    if (r->operation == Query) {
        if (!r->status) {
            return kIOReturnBadArgument;
        }
        *r->status = session->status_;
        return kIOReturnSuccess;
    }
    if (r->operation == Submit) {
        if (active_session_ || status_.state != Idle ||
            session->status_.state != Idle) {
            return kIOReturnBusy;
        }
    } else if (active_session_ != session) {
        return kIOReturnNotReady;
    }
    Request kernel = *r;
    kernel.session = nullptr;
    kernel.session_action = true;
    IOReturn result = dispatchGated(&kernel);
    if (r->operation == Submit && result == kIOReturnSuccess) {
        session->retain();
        active_session_ = session;
    }
    return result;
}

IOReturn InfernoMetalService::newSession(InfernoMetalSession **session)
{
    if (session) {
        *session = nullptr;
    }
    Request r = {};
    r.operation = NewSession;
    r.new_session = session;
    return dispatch(&r);
}

IOReturn InfernoMetalService::submitSession(InfernoMetalSession *session,
                                            const InfernoMetalCommand *command,
                                            const void *source,
                                            const void *input)
{
    if (!session) {
        return kIOReturnBadArgument;
    }
    Request r = {};
    r.operation = Submit;
    r.session = session;
    r.command = command;
    r.source = source;
    r.input = input;
    return dispatch(&r);
}

IOReturn InfernoMetalService::statusSession(InfernoMetalSession *session,
                                            Status *status)
{
    if (!session) {
        return kIOReturnBadArgument;
    }
    Request r = {};
    r.operation = Query;
    r.session = session;
    r.status = status;
    return dispatch(&r);
}

IOReturn InfernoMetalService::readSession(InfernoMetalSession *session,
                                          size_t offset, void *output,
                                          size_t size)
{
    if (!session) {
        return kIOReturnBadArgument;
    }
    Request r = {};
    r.operation = Read;
    r.session = session;
    r.offset = offset;
    r.output = output;
    r.size = size;
    return dispatch(&r);
}

IOReturn InfernoMetalService::acknowledgeSession(InfernoMetalSession *session)
{
    if (!session) {
        return kIOReturnBadArgument;
    }
    Request r = {};
    r.operation = Acknowledge;
    r.session = session;
    return dispatch(&r);
}

IOReturn InfernoMetalService::resetSession(InfernoMetalSession *session)
{
    if (!session) {
        return kIOReturnBadArgument;
    }
    Request r = {};
    r.operation = Reset;
    r.session = session;
    return dispatch(&r);
}

IOReturn InfernoMetalService::closeSession(InfernoMetalSession *session)
{
    if (!session) {
        return kIOReturnBadArgument;
    }
    Request r = {};
    r.operation = CloseSession;
    r.session = session;
    return dispatch(&r);
}

IOReturn InfernoMetalService::submitKernel(const InfernoMetalCommand *command,
                                           const void *source,
                                           const void *input)
{
    Request r = {};
    r.operation = Submit;
    r.command = command;
    r.source = source;
    r.input = input;
    return dispatch(&r);
}

IOReturn InfernoMetalService::getStatus(Status *status)
{
    Request r = {};
    r.operation = Query;
    r.status = status;
    return dispatch(&r);
}

IOReturn InfernoMetalService::readOutput(size_t offset, void *output,
                                         size_t size)
{
    Request r = {};
    r.operation = Read;
    r.offset = offset;
    r.output = output;
    r.size = size;
    return dispatch(&r);
}

IOReturn InfernoMetalService::acknowledge()
{
    Request r = {};
    r.operation = Acknowledge;
    return dispatch(&r);
}

IOReturn InfernoMetalService::resetCommand()
{
    Request r = {};
    r.operation = Reset;
    return dispatch(&r);
}

IOReturn InfernoMetalService::requestShutdown()
{
    Request r = {};
    r.operation = Shutdown;
    return dispatch(&r);
}

bool InfernoMetalService::willTerminate(IOService *provider,
                                        IOOptionBits options)
{
    requestShutdown();
    return IOService::willTerminate(provider, options);
}

bool InfernoMetalService::terminate(IOOptionBits options)
{
    /* Provider recursion first marks the child inactive; willTerminate then
     * starts its drain. Preserve this framework path, including its flags.
     */
    if (loop_ && loop_->inGate() && provider_recursing_) {
        return IOService::terminate(options);
    }
    if (!loop_ || loop_->onThread() || loop_->inGate()) {
        return false;
    }
    Request r = {};
    r.operation = CanTerminate;
    if (dispatch(&r) != kIOReturnSuccess) {
        return false;
    }
    return IOService::terminate(options);
}

bool InfernoMetalService::requestTerminate(IOService *provider,
                                           IOOptionBits options)
{
    Request r = {};
    r.operation = ProviderTerminate;
    r.provider = provider;
    r.options = options;
    return dispatch(&r) == kIOReturnSuccess;
}
