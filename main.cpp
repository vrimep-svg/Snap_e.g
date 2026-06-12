/*
 * SPDX-FileCopyrightText: Bosch Rexroth AG
 *
 * SPDX-License-Identifier: MIT
 */
/*
 An NTP message is a fixed 48-byte payload inside a UDP packet (port 123).
It contains metadata + multiple timestamps.

🔧 Structure (simplified)
Byte Offset   Field
-------------------------------------
0             LI | VN | Mode
1             Stratum
2             Poll
3             Precision
4–7           Root Delay
8–11          Root Dispersion
12–15         Reference ID
16–23         Reference Timestamp
24–31         Origin Timestamp   (T1)
32–39         Receive Timestamp  (T2)
40–47         Transmit Timestamp (T3)

Origin Timestamp (bytes 24–31)
T1 (what server received from you)Receive Timestamp (bytes 32–39) 
T2 — when server received your requestTransmit Timestamp (bytes 40–47) 
T3 — when server sent reply

Client                    Server
SendRequest
	  |------------ T1 --------->|
	  |             |  			 |T2 (recv)
	  |             |  			 |processing
	  |<---- T3 ----|------------|Send Reply
	T4|             |  			 |
Receive 
Reply 

*/
#include <csignal>
#include <thread>
#include <string>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

static constexpr uint64_t NTP_TIMESTAMP_DELTA = 2208988800ULL;
static constexpr uint64_t UNIX_TO_FILETIME_100NS = 116444736000000000ULL;

static double getCurrentUnixTime()
{
  using namespace std::chrono;
  auto now = std::chrono::system_clock::now();
  auto sinceEpoch = now.time_since_epoch();
  return duration_cast<duration<double>>(sinceEpoch).count();
}

static uint64_t unixToFiletime100ns(double unixSeconds)
{
  double integerPart;
  double fractionPart = std::modf(unixSeconds, &integerPart);
  uint64_t seconds = static_cast<uint64_t>(integerPart);
  uint64_t ticks100ns = seconds * 10000000ULL + static_cast<uint64_t>(std::llround(fractionPart * 1e7));
  return ticks100ns + UNIX_TO_FILETIME_100NS;
}

static double ntpToUnix(uint32_t seconds, uint32_t fraction)
{
  seconds = ntohl(seconds);
  fraction = ntohl(fraction);

  double sec = static_cast<double>(seconds) - static_cast<double>(NTP_TIMESTAMP_DELTA);
  double frac = static_cast<double>(fraction) / static_cast<double>(1ULL << 32);
  return sec + frac;
}

static void unixToNtpTimestamp(double unixSeconds, uint32_t& seconds, uint32_t& fraction)
{
  double ntp = unixSeconds + static_cast<double>(NTP_TIMESTAMP_DELTA);
  double integerPart;
  double fractionPart = std::modf(ntp, &integerPart);
  seconds = htonl(static_cast<uint32_t>(integerPart));
  fraction = htonl(static_cast<uint32_t>(fractionPart * static_cast<double>(1ULL << 32)));
}

static bool queryNtpServer(
  const char* serverIp,
  int64_t& offset100ns,
  int64_t& delay100ns,
  uint64_t& t1Filetime100ns,
  uint64_t& t2Filetime100ns,
  uint64_t& t3Filetime100ns,
  uint64_t& t4Filetime100ns)
{
  int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockfd < 0)
  {
    std::cout << "ERROR opening NTP socket: " << strerror(errno) << std::endl;
    return false;
  }

  struct sockaddr_in servAddr{};
  servAddr.sin_family = AF_INET;
  servAddr.sin_port = htons(123);
  if (inet_pton(AF_INET, serverIp, &servAddr.sin_addr) != 1)
  {
    std::cout << "ERROR invalid NTP server address: " << serverIp << std::endl;
    close(sockfd);
    return false;
  }

  uint8_t packet[48] = {0};
  packet[0] = 0x1B; // LI=0, VN=3, Mode=3 (client)

  double T1 = getCurrentUnixTime();
  uint32_t sec1;
  uint32_t frac1;
  unixToNtpTimestamp(T1, sec1, frac1);
  std::memcpy(&packet[40], &sec1, sizeof(sec1));
  std::memcpy(&packet[44], &frac1, sizeof(frac1));

  struct timeval timeout{};
  timeout.tv_sec = 2;
  timeout.tv_usec = 0;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  if (sendto(sockfd, packet, sizeof(packet), 0, reinterpret_cast<struct sockaddr*>(&servAddr), sizeof(servAddr)) < 0)
  {
    std::cout << "ERROR sending NTP request: " << strerror(errno) << std::endl;
    close(sockfd);
    return false;
  }

  socklen_t len = sizeof(servAddr);
  ssize_t received = recvfrom(sockfd, packet, sizeof(packet), 0, reinterpret_cast<struct sockaddr*>(&servAddr), &len);
  double T4 = getCurrentUnixTime();
  close(sockfd);

  if (received < static_cast<ssize_t>(sizeof(packet)))
  {
    std::cout << "ERROR NTP reply too short" << std::endl;
    return false;
  }

  uint32_t sec2;
  uint32_t frac2;
  uint32_t sec3;
  uint32_t frac3;
  std::memcpy(&sec2, packet + 32, sizeof(sec2));
  std::memcpy(&frac2, packet + 36, sizeof(frac2));
  std::memcpy(&sec3, packet + 40, sizeof(sec3));
  std::memcpy(&frac3, packet + 44, sizeof(frac3));

  double T2 = ntpToUnix(sec2, frac2);
  double T3 = ntpToUnix(sec3, frac3);

  double offsetSeconds = ((T2 - T1) + (T3 - T4)) / 2.0;
  double delaySeconds = (T4 - T1) - (T3 - T2);

  offset100ns = static_cast<int64_t>(std::llround(offsetSeconds * 1e7));
  delay100ns = static_cast<int64_t>(std::llround(delaySeconds * 1e7));
  t1Filetime100ns = unixToFiletime100ns(T1);
  t2Filetime100ns = unixToFiletime100ns(T2);
  t3Filetime100ns = unixToFiletime100ns(T3);
  t4Filetime100ns = unixToFiletime100ns(T4);
  return true;
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

    const char* ntpServer = "192.168.1.218"; // local NTP server
    int64_t lastOffset100ns = 0;
    int64_t lastDelay100ns = 0;
    uint64_t lastT1Filetime100ns = getCurrentTimestampFiletime();
    uint64_t lastT2Filetime100ns = getCurrentTimestampFiletime();
    uint64_t lastT3Filetime100ns = getCurrentTimestampFiletime();
    uint64_t lastT4Filetime100ns = getCurrentTimestampFiletime();

    // Inner loop
    std::cout << "INFO Running endless loop - will be ended on connection error or user input of Ctrl+C" << std::endl;
	while (g_endProcess == false && provider->isConnected())
	{
		int64_t offset100ns = 0;
		int64_t delay100ns = 0;
    uint64_t t1Filetime100ns = 0;
    uint64_t t2Filetime100ns = 0;
    uint64_t t3Filetime100ns = 0;
    uint64_t t4Filetime100ns = 0;
		if (queryNtpServer(ntpServer, offset100ns, delay100ns, t1Filetime100ns, t2Filetime100ns, t3Filetime100ns, t4Filetime100ns))
		{
			lastOffset100ns = offset100ns;
			lastDelay100ns = delay100ns;
      lastT1Filetime100ns = t1Filetime100ns;
      lastT2Filetime100ns = t2Filetime100ns;
      lastT3Filetime100ns = t3Filetime100ns;
      lastT4Filetime100ns = t4Filetime100ns;
			double offsetSeconds = static_cast<double>(offset100ns) / 1e7;
			double delaySeconds = static_cast<double>(delay100ns) / 1e7;
			std::cout << "INFO NTP offset: " << offsetSeconds << " seconds, delay: " << delaySeconds << " seconds" << std::endl;
		}
		else
		{
			double offsetSeconds = static_cast<double>(lastOffset100ns) / 1e7;
			double delaySeconds = static_cast<double>(lastDelay100ns) / 1e7;
			std::cout << "WARNING NTP query failed, keeping previous offset: " << offsetSeconds << " seconds, delay: " << delaySeconds << " seconds" << std::endl;
		}

    providerNodeStatic->updateData(lastOffset100ns, lastT4Filetime100ns, lastT3Filetime100ns,
                 lastT1Filetime100ns, lastT2Filetime100ns, lastT3Filetime100ns, lastT4Filetime100ns,
                 lastDelay100ns);

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
