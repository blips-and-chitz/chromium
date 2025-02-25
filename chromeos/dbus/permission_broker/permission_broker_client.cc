// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/dbus/permission_broker/permission_broker_client.h"

#include <stdint.h>
#include <utility>

#include "base/bind.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "chromeos/dbus/permission_broker/fake_permission_broker_client.h"
#include "dbus/bus.h"
#include "dbus/message.h"
#include "dbus/object_proxy.h"
#include "third_party/cros_system_api/dbus/service_constants.h"

using permission_broker::kCheckPathAccess;
using permission_broker::kOpenPath;
using permission_broker::kPermissionBrokerInterface;
using permission_broker::kPermissionBrokerServiceName;
using permission_broker::kPermissionBrokerServicePath;
using permission_broker::kReleaseTcpPort;
using permission_broker::kReleaseUdpPort;
using permission_broker::kRequestTcpPortAccess;
using permission_broker::kRequestUdpPortAccess;

namespace chromeos {

namespace {

const char kNoResponseError[] = "org.chromium.Error.NoResponse";

PermissionBrokerClient* g_instance = nullptr;

}  // namespace

class PermissionBrokerClientImpl : public PermissionBrokerClient {
 public:
  PermissionBrokerClientImpl() = default;
  ~PermissionBrokerClientImpl() override = default;

  void CheckPathAccess(const std::string& path,
                       const ResultCallback& callback) override {
    dbus::MethodCall method_call(kPermissionBrokerInterface, kCheckPathAccess);
    dbus::MessageWriter writer(&method_call);
    writer.AppendString(path);
    proxy_->CallMethod(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&PermissionBrokerClientImpl::OnResponse,
                       weak_ptr_factory_.GetWeakPtr(), callback));
  }

  void OpenPath(const std::string& path,
                const OpenPathCallback& callback,
                const ErrorCallback& error_callback) override {
    dbus::MethodCall method_call(kPermissionBrokerInterface, kOpenPath);
    dbus::MessageWriter writer(&method_call);
    writer.AppendString(path);
    proxy_->CallMethodWithErrorCallback(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&PermissionBrokerClientImpl::OnOpenPathResponse,
                       weak_ptr_factory_.GetWeakPtr(), callback),
        base::BindOnce(&PermissionBrokerClientImpl::OnError,
                       weak_ptr_factory_.GetWeakPtr(), error_callback));
  }

  void RequestTcpPortAccess(uint16_t port,
                            const std::string& interface,
                            int lifeline_fd,
                            const ResultCallback& callback) override {
    dbus::MethodCall method_call(kPermissionBrokerInterface,
                                 kRequestTcpPortAccess);
    dbus::MessageWriter writer(&method_call);
    writer.AppendUint16(port);
    writer.AppendString(interface);
    writer.AppendFileDescriptor(lifeline_fd);
    proxy_->CallMethod(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&PermissionBrokerClientImpl::OnResponse,
                       weak_ptr_factory_.GetWeakPtr(), callback));
  }

  void RequestUdpPortAccess(uint16_t port,
                            const std::string& interface,
                            int lifeline_fd,
                            const ResultCallback& callback) override {
    dbus::MethodCall method_call(kPermissionBrokerInterface,
                                 kRequestUdpPortAccess);
    dbus::MessageWriter writer(&method_call);
    writer.AppendUint16(port);
    writer.AppendString(interface);
    writer.AppendFileDescriptor(lifeline_fd);
    proxy_->CallMethod(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&PermissionBrokerClientImpl::OnResponse,
                       weak_ptr_factory_.GetWeakPtr(), callback));
  }

  void ReleaseTcpPort(uint16_t port,
                      const std::string& interface,
                      const ResultCallback& callback) override {
    dbus::MethodCall method_call(kPermissionBrokerInterface, kReleaseTcpPort);
    dbus::MessageWriter writer(&method_call);
    writer.AppendUint16(port);
    writer.AppendString(interface);
    proxy_->CallMethod(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&PermissionBrokerClientImpl::OnResponse,
                       weak_ptr_factory_.GetWeakPtr(), callback));
  }

  void ReleaseUdpPort(uint16_t port,
                      const std::string& interface,
                      const ResultCallback& callback) override {
    dbus::MethodCall method_call(kPermissionBrokerInterface, kReleaseUdpPort);
    dbus::MessageWriter writer(&method_call);
    writer.AppendUint16(port);
    writer.AppendString(interface);
    proxy_->CallMethod(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&PermissionBrokerClientImpl::OnResponse,
                       weak_ptr_factory_.GetWeakPtr(), callback));
  }

  void Init(dbus::Bus* bus) {
    proxy_ =
        bus->GetObjectProxy(kPermissionBrokerServiceName,
                            dbus::ObjectPath(kPermissionBrokerServicePath));
  }

 private:
  // Handle a DBus response from the permission broker, invoking the callback
  // that the method was originally called with with the success response.
  void OnResponse(const ResultCallback& callback, dbus::Response* response) {
    if (!response) {
      LOG(WARNING) << "Access request method call failed.";
      callback.Run(false);
      return;
    }

    bool result = false;
    dbus::MessageReader reader(response);
    if (!reader.PopBool(&result))
      LOG(WARNING) << "Could not parse response: " << response->ToString();
    callback.Run(result);
  }

  void OnOpenPathResponse(const OpenPathCallback& callback,
                          dbus::Response* response) {
    base::ScopedFD fd;
    dbus::MessageReader reader(response);
    if (!reader.PopFileDescriptor(&fd))
      LOG(WARNING) << "Could not parse response: " << response->ToString();
    callback.Run(std::move(fd));
  }

  void OnError(const ErrorCallback& callback, dbus::ErrorResponse* response) {
    std::string error_name;
    std::string error_message;
    if (response) {
      dbus::MessageReader reader(response);
      error_name = response->GetErrorName();
      reader.PopString(&error_message);
    } else {
      error_name = kNoResponseError;
    }
    callback.Run(error_name, error_message);
  }

  dbus::ObjectProxy* proxy_ = nullptr;

  // Note: This should remain the last member so that it will be destroyed
  // first, invalidating its weak pointers, before the other members are
  // destroyed.
  base::WeakPtrFactory<PermissionBrokerClientImpl> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(PermissionBrokerClientImpl);
};

PermissionBrokerClient::PermissionBrokerClient() {
  DCHECK(!g_instance);
  g_instance = this;
}

PermissionBrokerClient::~PermissionBrokerClient() {
  DCHECK_EQ(this, g_instance);
  g_instance = nullptr;
}

// static
void PermissionBrokerClient::Initialize(dbus::Bus* bus) {
  DCHECK(bus);
  (new PermissionBrokerClientImpl())->Init(bus);
}

// static
void PermissionBrokerClient::InitializeFake() {
  new FakePermissionBrokerClient();
}

// static
void PermissionBrokerClient::Shutdown() {
  DCHECK(g_instance);
  delete g_instance;
}

// static
PermissionBrokerClient* PermissionBrokerClient::Get() {
  return g_instance;
}

}  // namespace chromeos
