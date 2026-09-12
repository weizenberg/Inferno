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

#ifndef INFERNO_METAL_SERVICE_H
#define INFERNO_METAL_SERVICE_H

#include "iokit-transport.h"
#include <IOKit/IOService.h>

class IOCommandGate;
class IOUserClient;
class IOTimerEventSource;
class IOWorkLoop;
class OSDictionary;
class InfernoMetalUserClient;
class InfernoMetalSession;

class InfernoMetalService : public IOService {
    OSDeclareDefaultStructors(InfernoMetalService);

  public:
    enum State {
        Idle,
        Submitted,
        Completed,
        Resetting,
        Faulted,
        Draining,
        Drained,
        Stopped
    };
    struct Status {
        State state;
        ImtlResult transport_result;
        /* Most recent timer-arm result; acknowledge/new submit clears it. */
        IOReturn timer_error;
        ImtlCompletion completion;
    };

    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOWorkLoop *getWorkLoop() const override;
    bool willTerminate(IOService *provider, IOOptionBits options) override;
    bool requestTerminate(IOService *provider, IOOptionBits options) override;
    bool terminate(IOOptionBits options = 0) override;
    IOReturn
    newUserClient(task_t owningTask, void *securityID, UInt32 type,
                  OSDictionary *properties,
                  LIBKERN_RETURNS_RETAINED IOUserClient **handler) override;

    /* Kernel callers retain the service for each call; no hardware interrupts
     * or user pointers. Every method serializes through the command gate.
     */
    IOReturn submitKernel(const InfernoMetalCommand *command,
                          const void *source, const void *input);
    IOReturn getStatus(Status *status);
    IOReturn readOutput(size_t offset, void *output, size_t size);
    IOReturn acknowledge();
    IOReturn resetCommand();
    /* Stop accepting commands and begin asynchronous destruction. NotReady
     * requires a later retry or getStatus; timer failure retains everything.
     * Only after success may a non-workloop caller invoke direct terminate().
     */
    IOReturn requestShutdown();

    /* Session callers retain both service and session across each call.
     * Each completed result belongs to its submitting session until ACK or
     * reset. Close revokes the session before retiring any outstanding DMA.
     */
    IOReturn newSession(InfernoMetalSession **session);
    IOReturn submitSession(InfernoMetalSession *, const InfernoMetalCommand *,
                           const void *source, const void *input);
    IOReturn statusSession(InfernoMetalSession *, Status *);
    IOReturn readSession(InfernoMetalSession *, size_t offset, void *, size_t);
    IOReturn acknowledgeSession(InfernoMetalSession *);
    IOReturn resetSession(InfernoMetalSession *);
    /* A valid session is irrevocably closed even when the return value reports
     * NotReady (draining) or a reset error. Caller may release its reference on
     * every such result; retry/status of retirement belongs to the service.
     * Closing again is permitted but never reopens the session.
     */
    IOReturn closeSession(InfernoMetalSession *);

  private:
    enum Operation {
        Submit,
        Query,
        Read,
        Acknowledge,
        Reset,
        Shutdown,
        CanTerminate,
        ProviderTerminate,
        NewSession,
        CloseSession
    };
    struct Request {
        Operation operation;
        const InfernoMetalCommand *command;
        const void *source;
        const void *input;
        void *output;
        size_t offset;
        size_t size;
        Status *status;
        IOService *provider;
        IOOptionBits options;
        InfernoMetalSession *session;
        InfernoMetalSession **new_session;
        bool session_action;
    };
    IOWorkLoop *loop_ = nullptr;
    IOCommandGate *gate_ = nullptr;
    IOTimerEventSource *timer_ = nullptr;
    IOService *provider_ = nullptr;
    ImtlIokitTransport *owner_ = nullptr;
    InfernoMetalSession *active_session_ = nullptr;
    bool provider_open_ = false;
    bool gate_added_ = false;
    bool timer_added_ = false;
    bool provider_recursing_ = false;
    Status status_ = { Stopped, IMTL_OK, kIOReturnSuccess, { 0, 0 } };

    static IOReturn gateAction(OSObject *, void *, void *, void *, void *);
    static IOReturn stopAction(OSObject *, void *, void *, void *, void *);
    static void timerAction(OSObject *, IOTimerEventSource *);
    IOReturn dispatch(Request *request);
    IOReturn dispatchGated(Request *request);
    IOReturn dispatchSessionGated(Request *request);
    void syncSessionGated();
    void refreshGated();
    IOReturn shutdownGated();
    void armGated();
    void removeSources();
};

class InfernoMetalSession : public OSObject {
    OSDeclareDefaultStructors(InfernoMetalSession);
    friend class InfernoMetalService;

  private:
    /* Weak identity only. Caller retains its service; callbacks never use it.
     */
    InfernoMetalService *service_ = nullptr;
    InfernoMetalService::Status status_ = {
        InfernoMetalService::Idle, IMTL_OK, kIOReturnSuccess, { 0, 0 }
    };
    bool closing_ = false;
};
#endif
