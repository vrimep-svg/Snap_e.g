/*
 * SPDX-FileCopyrightText: Bosch Rexroth AG
 *
 * SPDX-License-Identifier: MIT
 */

#include <csignal>
#include <thread>
#include <string>
#include <chrono>

#include "ctrlx_datalayer_helper.h"
#include "providerNodeAllData.h"

static std::string g_dataLayerAddrBase = "TimeObserver/";

static bool g_errorReported = false;

// Add signal handling so we are able to abort the program by sending SIGINT
static bool g_endProcess = false;
static void signalHandler(int signal)
{
  std::cout << "signal: " << signal << std::endl;
  g_endProcess = true;
}

static uint64_t getCurrentTimestampFiletime()
{
  using namespace std::chrono;
  constexpr uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
  auto now = system_clock::now();
  auto since_epoch = now.time_since_epoch();
  auto ticks100ns = duration_cast<duration<uint64_t, std::ratio<1, 10000000>>>(since_epoch).count();
  return ticks100ns + EPOCH_DIFF_100NS;
}

int main(void)
{

  // Prepare signal structure to interrupt the endless loop with Ctrl+C (SIGINT)
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);
  std::signal(SIGABRT, signalHandler);

  comm::datalayer::DatalayerSystem datalayerSystem;
  datalayerSystem.start(false);

  // Outer loop
  for (;;)
  {

    // ip="192.168.1.1" or any other IP address if a ctrlX CORE or ctrlX COREvirtual
    // ip="10.0.2.2"    ctrlX COREvirtual with port forwarding
    auto* provider = getProvider(datalayerSystem);

    if (provider == nullptr) {
      // Wait before (re)connecting to the ctrlX Data Layer.
      if (g_endProcess)
      {
        std::cout << "INFO Terminating process due to user input of Ctrl+C (signal SIGINT)" << std::endl;
        datalayerSystem.stop();
        return 0;
      }

      std::cout << "INFO Wait before (re)connecting to the ctrlX Data Layer " << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(10));
      continue;
    }

    g_errorReported = false;

    std::cout << "INFO Creating ctrlX Data Layer node branch " << g_dataLayerAddrBase << std::endl;

    auto providerNodeStatic = new ProviderNodeAllData(provider, g_dataLayerAddrBase, false);
    providerNodeStatic->registerNodes();


    // Inner loop
    std::cout << "INFO Running endless loop - will be ended on connection error or user input of Ctrl+C" << std::endl;
	while (g_endProcess == false && provider->isConnected())
	{
		uint64_t t = getCurrentTimestampFiletime();

		providerNodeStatic->updateData(0, t, t);

		std::this_thread::sleep_for(std::chrono::seconds(10));
	}

    delete providerNodeStatic;

    provider->stop();
    delete provider;

    if (g_endProcess)
    {
      std::cout << "INFO Terminating process due to user input of Ctrl+C (signal SIGINT)" << std::endl;
      datalayerSystem.stop();
      return 0;
    }

    // ctrlX Data Layer is disconnected
    if (!g_errorReported)
    {
      g_errorReported = true;
      std::cout << "ERROR Connection to ctrlX Data Layer failed - retry every 10 seconds" << std::endl;
    }
  }
}
