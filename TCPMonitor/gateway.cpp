#include <opendnp3/DNP3Manager.h>

#include <opendnp3/outstation/OutstationStackConfig.h>
#include <opendnp3/outstation/UpdateBuilder.h>
#include <opendnp3/outstation/SimpleCommandHandler.h>
#include <opendnp3/outstation/DefaultOutstationApplication.h>

#include <opendnp3/channel/PrintingChannelListener.h>

#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

using namespace opendnp3;

/* -------------------- SHARED STATE -------------------- */

struct SensorState
{
    float temp = 0.0f;
    float hum  = 0.0f;
    int HrStatusSt = 0;
    uint32_t keypad = 0;
    std::chrono::steady_clock::time_point last_update =
        std::chrono::steady_clock::now() - std::chrono::hours(24);
};

static SensorState g_state;
static std::mutex g_mutex;

/* -------------------- INGEST THREAD -------------------- */

static uint32_t key_to_counter(char k)
{
    if (k >= '0' && k <= '9') return k - '0';
    return static_cast<uint32_t>(k);
}

static void ingest_thread(uint16_t port)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(server, (sockaddr*)&addr, sizeof(addr));
    listen(server, 10);

    std::cout << "[INGEST] Listening on " << port << "\n";

    while (true)
    {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;

        char buf[256]{};
        int n = read(client, buf, sizeof(buf)-1);
        close(client);

        if (n <= 0) continue;

        int dev = -1;
        char type[16]{};

        if (sscanf(buf, "DEV=%d,TYPE=%15[^,]", &dev, type) != 2)
        {
            std::cout << "[INGEST] Bad header: " << buf << "\n";
            continue;
        }

        std::lock_guard<std::mutex> lock(g_mutex);

        if (strcmp(type, "ENV") == 0)
        {
            float t, h;
            if (sscanf(buf,
                "DEV=%*d,TYPE=ENV,TEMP=%f,HUM=%f", &t, &h) == 2)
            {
                g_state.temp = t;
                g_state.hum  = h;
                g_state.last_update = std::chrono::steady_clock::now();
                std::cout << "[ENV] T=" << t << " H=" << h << "\n";
            }
        }
        else if (strcmp(type, "KEYPAD") == 0)
        {
            char key;
            if (sscanf(buf,
                "DEV=%*d,TYPE=KEYPAD,KEY=%c", &key) == 1)
            {
                g_state.keypad = key_to_counter(key);
                g_state.last_update = std::chrono::steady_clock::now();
                std::cout << "[KEYPAD] key=" << key << "\n";
            }
        }
        else if (strcmp(type, "SENSOR") == 0)
        {
        	int HrStatus;
        	if(sscanf(buf, "DEV=%*d,TYPE=SENSOR,GPIO=%*d,STATE=%d", &HrStatus) == 1)
        	{
        		g_state.HrStatusSt = HrStatus;
        		g_state.last_update = std::chrono::steady_clock::now();
        		std::cout << "[HCSR501] STATE: " << HrStatus << "\n";
        	}
        }
        else if (strcmp(type, "T") == 0)
        {
        	if (sscanf(buf, "PASSCODE_CORRECT") == 1)
        	{
        		std::cout << "PASSCODE CORRECT" << "\n";
        	}
        	else
        	{
        		std::cout << "PASSCODE INCORRECT" << "\n";
        	}
        }
       	
        
        else
        {
            std::cout << "[INGEST] Unknown TYPE=" << type << "\n";
        }
    }
}

/* -------------------- MAIN -------------------- */

int main()
{
    DNP3Manager manager(1);

    auto channel = manager.AddTCPServer(
        "outstation",
        levels::NORMAL,
        ServerAcceptMode::CloseExisting,
        IPEndpoint("0.0.0.0", 9000), /*Sends to LOCAL master -> config IP to your Local*/
        PrintingChannelListener::Create()
    );

    OutstationStackConfig config;
    config.database.analog_input[0] = AnalogConfig();   // TEMP
    config.database.analog_input[1] = AnalogConfig();   // HUM
    config.database.binary_input[0] = BinaryConfig();   // ONLINE
    config.database.counter[0]      = CounterConfig();  // KEYPAD
    config.database.analog_input[2] = AnalogConfig();   // MOTION SENSOR

    auto outstation = channel->AddOutstation(
        "station",
        std::make_shared<SimpleCommandHandler>(CommandStatus::SUCCESS),
        std::make_shared<DefaultOutstationApplication>(),
        config
    );

    outstation->Enable();
    std::cout << "[DNP3] Outstation on port 9000\n";

    std::thread ingest(ingest_thread, 9100); 

    while (true)
    {
        SensorState local;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            local = g_state;
        }

        bool online =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - local.last_update
            ).count() < 10;

        UpdateBuilder b;
        b.Update(Analog(local.temp), 0);
        b.Update(Analog(local.hum),  1);
        b.Update(Binary(online),     0);
        b.Update(Counter(local.keypad), 0);
        b.Update(Analog(local.HrStatusSt), 2);

        outstation->Apply(b.Build());
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    ingest.join();
}

