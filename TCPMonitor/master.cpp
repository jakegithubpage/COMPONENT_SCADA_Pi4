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
        { IPEndpoint("x.x.x.x", 9000) },  // gateway IP
        "0.0.0.0",                        // master machine IP
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
    std::cout << "BI0 = A Sensor is Online " << "\n" << "AI0 = Temp" << "\n" << "AI1 = Hum" << "\n" << "AI2 = Motion Sensor" << "\n" << "AI3 = Left Active Rotary" << "\n" << "AI4 = Right Active Rotary" << "\n" << "CI0 = Keypad press" << "\n";

    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(5));
}
