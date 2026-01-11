#include <opendnp3/DNP3Manager.h>

#include <opendnp3/master/MasterStackConfig.h>
#include <opendnp3/master/DefaultMasterApplication.h>
#include <opendnp3/master/PrintingSOEHandler.h>

#include <opendnp3/channel/PrintingChannelListener.h>

#include <iostream>
#include <thread>

using namespace opendnp3;

int main()
{
    DNP3Manager manager(1);

    auto channel = manager.AddTCPClient(
        "master",
        levels::NORMAL,
        ChannelRetry::Default(),
        { IPEndpoint("x.x.x.x", 9000) },  /* gateway IP CHANGE DEPENDING ON YOUR IP ADDR -> SET PORT 9000 (9100 used for TCP) */
        "0.0.0.0",                        // change to your master IP
        PrintingChannelListener::Create()
    );

    MasterStackConfig config;
    config.master.disableUnsolOnStartup = true;

    auto soe = PrintingSOEHandler::Create();
    auto app = std::make_shared<DefaultMasterApplication>();

    auto master = channel->AddMaster(
        "master",
        soe,
        app,
        config
    );

    master->Enable();

    master->AddClassScan(
        ClassField::AllClasses(),
        TimeDuration::Seconds(2),
        soe,
        TaskConfig::Default()
    );

    std::cout << "[MASTER] Running\n";
    std::cout << "BI0 = Sensor Online " << "\n" << "AI0 = Temp" << "\n" << "AI1 = Hum" << "\n" << "CI0 = Keypad press" << "AI2 = Motion Sensor" << "\n";

    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(5));
}

