#include <base/at_exit.h>
#include <base/command_line.h>

#include "host_service.h"

int main(int argc, char** argv) {
    base::AtExitManager at_exit_;
    base::CommandLine::Init(argc, argv);

    remoting::HostService* service = remoting::HostService::GetInstance();
    service->InitWithCommandLine(base::CommandLine::ForCurrentProcess());

    return service->Run();
}