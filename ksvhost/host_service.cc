// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file implements the Windows service controlling Me2Me host processes
// running within user sessions.

#include "host_service.h"

#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>

#include "base/base_paths.h"
#include "base/base_switches.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread.h"
#include "base/win/message_window.h"
#include "base/win/scoped_com_initializer.h"

//#include "host_exit_codes.h"
#include "wts_terminal_observer.h"

namespace remoting {

HostService* HostService::GetInstance() {
  return base::Singleton<HostService>::get();
}

bool HostService::InitWithCommandLine(const base::CommandLine* command_line) {
  base::CommandLine::StringVector args = command_line->GetArgs();
  return true;
}

int HostService::Run() {
  return HostService::RunAsService();
}

bool HostService::AddWtsTerminalObserver(const std::string& terminal_id,
                                         WtsTerminalObserver* observer) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  RegisteredObserver registered_observer;
  registered_observer.terminal_id = terminal_id;
  registered_observer.session_id = kInvalidSessionId;
  registered_observer.observer = observer;

  bool session_id_found = false;
  std::list<RegisteredObserver>::const_iterator i;
  for (i = observers_.begin(); i != observers_.end(); ++i) {
    // Get the attached session ID from another observer watching the same WTS
    // console if any.
    if (i->terminal_id == terminal_id) {
      registered_observer.session_id = i->session_id;
      session_id_found = true;
    }

    // Check that |observer| hasn't been registered already.
    if (i->observer == observer)
      return false;
  }

  // If |terminal_id| is new, enumerate all sessions to see if there is one
  // attached to |terminal_id|.
  if (!session_id_found)
    registered_observer.session_id = LookupSessionId(terminal_id);

  observers_.push_back(registered_observer);

  if (registered_observer.session_id != kInvalidSessionId) {
    observer->OnSessionAttached(registered_observer.session_id);
  }

  return true;
}

void HostService::RemoveWtsTerminalObserver(WtsTerminalObserver* observer) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  std::list<RegisteredObserver>::const_iterator i;
  for (i = observers_.begin(); i != observers_.end(); ++i) {
    if (i->observer == observer) {
      observers_.erase(i);
      return;
    }
  }
}

HostService::HostService() :
  service_status_handle_(0),
  stopped_event_(true, false),
  weak_factory_(this) {
}

HostService::~HostService() {
}

void HostService::OnSessionChange(uint32_t event, uint32_t session_id) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DCHECK_NE(session_id, kInvalidSessionId);

  // Process only attach/detach notifications.
  if (event != WTS_CONSOLE_CONNECT && event != WTS_CONSOLE_DISCONNECT &&
      event != WTS_REMOTE_CONNECT && event != WTS_REMOTE_DISCONNECT) {
    return;
  }

  // Assuming that notification can arrive later query the current state of
  // |session_id|.
  std::string terminal_id;
  bool attached = LookupTerminalId(session_id, &terminal_id);

  std::list<RegisteredObserver>::iterator i = observers_.begin();
  while (i != observers_.end()) {
    std::list<RegisteredObserver>::iterator next = i;
    ++next;

    // Issue a detach notification if the session was detached from a client or
    // if it is now attached to a different client.
    if (i->session_id == session_id &&
        (!attached || !(i->terminal_id == terminal_id))) {
      i->session_id = kInvalidSessionId;
      i->observer->OnSessionDetached();
      i = next;
      continue;
    }

    // The client currently attached to |session_id| was attached to a different
    // session before. Reconnect it to |session_id|.
    if (attached && i->terminal_id == terminal_id &&
        i->session_id != session_id) {
      WtsTerminalObserver* observer = i->observer;

      if (i->session_id != kInvalidSessionId) {
        i->session_id = kInvalidSessionId;
        i->observer->OnSessionDetached();
      }

      // Verify that OnSessionDetached() above didn't remove |observer|
      // from the list.
      std::list<RegisteredObserver>::iterator j = next;
      --j;
      if (j->observer == observer) {
        j->session_id = session_id;
        observer->OnSessionAttached(session_id);
      }
    }

    i = next;
  }
}

const wchar_t kWindowsServiceName[] = L"chromoting";

int HostService::RunAsService() {
  SERVICE_TABLE_ENTRYW dispatch_table[] = {
    { const_cast<LPWSTR>(kWindowsServiceName), &HostService::ServiceMain },
    { nullptr, nullptr }
  };

  if (!StartServiceCtrlDispatcherW(dispatch_table)) {
    PLOG(ERROR) << "Failed to connect to the service control manager";
    return -1;
  }

  // Wait until the service thread completely exited to avoid concurrent
  // teardown of objects registered with base::AtExitManager and object
  // destoyed by the service thread.
  stopped_event_.Wait();

  return 0;
}

void HostService::RunAsServiceImpl() {
  base::MessageLoopForUI message_loop;
  base::RunLoop run_loop;
  main_task_runner_ = message_loop.task_runner();
  weak_ptr_ = weak_factory_.GetWeakPtr();

  // Register the service control handler.
  service_status_handle_ = RegisterServiceCtrlHandlerExW(
      kWindowsServiceName, &HostService::ServiceControlHandler, this);
  if (service_status_handle_ == 0) {
    PLOG(ERROR) << "Failed to register the service control handler";
    return;
  }

  // Report running status of the service.
  SERVICE_STATUS service_status;
  ZeroMemory(&service_status, sizeof(service_status));
  service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service_status.dwCurrentState = SERVICE_RUNNING;
  service_status.dwControlsAccepted = SERVICE_ACCEPT_SHUTDOWN |
                                      SERVICE_ACCEPT_STOP |
                                      SERVICE_ACCEPT_SESSIONCHANGE;
  service_status.dwWin32ExitCode = 0;
  if (!SetServiceStatus(service_status_handle_, &service_status)) {
    PLOG(ERROR)
        << "Failed to report service status to the service control manager";
    return;
  }

  // Initialize COM.
  base::win::ScopedCOMInitializer com_initializer;
  if (!com_initializer.succeeded())
    return;

  quit_closure_ = run_loop.QuitClosure();

  // Run the service.
  run_loop.Run();
  weak_factory_.InvalidateWeakPtrs();

  // Tell SCM that the service is stopped.
  service_status.dwCurrentState = SERVICE_STOPPED;
  service_status.dwControlsAccepted = 0;
  if (!SetServiceStatus(service_status_handle_, &service_status)) {
    PLOG(ERROR)
        << "Failed to report service status to the service control manager";
    return;
  }
}

// static
DWORD WINAPI HostService::ServiceControlHandler(DWORD control,
                                                DWORD event_type,
                                                LPVOID event_data,
                                                LPVOID context) {
  HostService* self = reinterpret_cast<HostService*>(context);
  switch (control) {
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;

    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_STOP:
        self->main_task_runner_->PostTask(FROM_HERE, self->quit_closure_);
      return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
      self->main_task_runner_->PostTask(FROM_HERE, base::Bind(
          &HostService::OnSessionChange, self->weak_ptr_, event_type,
          reinterpret_cast<WTSSESSION_NOTIFICATION*>(event_data)->dwSessionId));
      return NO_ERROR;

    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

// static
VOID WINAPI HostService::ServiceMain(DWORD argc, WCHAR* argv[]) {
  HostService* self = HostService::GetInstance();

  // Run the service.
  self->RunAsServiceImpl();

  // Release the control handler and notify the main thread that it can exit
  // now.
  self->stopped_event_.Signal();
}
}  // namespace remoting
