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

#ifndef INFERNO_METAL_USER_CLIENT_H
#define INFERNO_METAL_USER_CLIENT_H

#include "standard-headers/inferno/metal-user.h"
#include <IOKit/IOUserClient.h>

class InfernoMetalService;
class InfernoMetalSession;
class IOWorkLoop;

class InfernoMetalUserClient : public IOUserClient {
    OSDeclareDefaultStructors(InfernoMetalUserClient);

  public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch = nullptr,
                            OSObject *target = nullptr,
                            void *reference = nullptr) override;

  private:
    struct MethodRequest {
        uint32_t selector;
        IOExternalMethodArguments *arguments;
        uint32_t scalar_output_capacity;
        uint32_t inline_output_capacity;
        uint32_t descriptor_output_capacity;
    };

    InfernoMetalService *service_ = nullptr;
    IOWorkLoop *loop_ = nullptr;
    InfernoMetalSession *session_ = nullptr;
    bool closing_ = false;

    static IOReturn methodAction(OSObject *, void *, void *, void *, void *);
    static IOReturn closeAction(OSObject *, void *, void *, void *, void *);
    IOReturn dispatchMethod(MethodRequest *request);
    IOReturn closeGated();
};

#endif
