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

#include <string>
#include <chrono>
#include <sstream>
#include <iomanip>

#include "providerNodeAllData.h"
#include "sampleSchema_generated.h"
#include "ctrlx_datalayer_helper.h"

static uint64_t getCurrentTimestampFiletime()
{
  using namespace std::chrono;
  constexpr uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
  auto now = system_clock::now();
  auto since_epoch = now.time_since_epoch();
  auto ticks100ns = duration_cast<duration<uint64_t, std::ratio<1, 10000000>>>(since_epoch).count();
  return ticks100ns + EPOCH_DIFF_100NS;
}

DataContainer* ProviderNodeAllData::getDataContainer(const std::string& address)
{
  for (auto dataContainer : m_dataContainers)
  {
    if (dataContainer->m_address == address)
    {
      return dataContainer;
    }
  }
  return NULL;
}

comm::datalayer::DlResult ProviderNodeAllData::createDataContainer(
  const std::string& address,
  const comm::datalayer::Variant& data)
{
  if (getDataContainer(address) != nullptr)
  {
    // We return NO error here
    return comm::datalayer::DlResult::DL_OK;
  }

  auto metadata = createMetadata(data, address);
  auto dataContainer = new DataContainer(address, data, metadata);

  m_dataContainers.emplace_back(dataContainer);

  // We register the address of our datacontainer now so that the ctrlX Data Layer will show it as "virtual node".
  // The "virtual node" has as provider this instance.
  return m_provider->registerNode(address, this);
}

void ProviderNodeAllData::createDataContainer(
  comm::datalayer::DlResult setValueResult,
  const std::string& addressNode,
  const comm::datalayer::Variant& data)
{
  const std::string address = m_addressBase + addressNode;
  if (comm::datalayer::DlResult::DL_OK != setValueResult)
  {
    std::cout << "ERROR - Data could not be set: " << address;
    return;
  }
  if (createDataContainer(address, data) != comm::datalayer::DlResult::DL_OK)
  {
    std::cout << "ERROR - DataContainer could not be created: " << address;
  }
}

comm::datalayer::Variant ProviderNodeAllData::createMetadata(const comm::datalayer::Variant& data, const std::string& address)
{

  flatbuffers::FlatBufferBuilder builder;
  auto description = builder.CreateString("This is a description for " + address);
  comm::datalayer::NodeClass nodeClass = comm::datalayer::NodeClass_Variable;

  bool isBaseFolder = address == m_addressBase;
  if (isBaseFolder)
  {
    nodeClass = comm::datalayer::NodeClass_Folder;
  }

  comm::datalayer::AllowedOperationsBuilder allowedOperations(builder);
  allowedOperations.add_read(!isBaseFolder);
  allowedOperations.add_write(!isBaseFolder && m_dynamic);
  allowedOperations.add_create(!isBaseFolder && m_dynamic);
  allowedOperations.add_delete_(!isBaseFolder && m_dynamic);
  auto operations = allowedOperations.Finish();

  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<comm::datalayer::Reference>>> references;
  if (data.getType() == comm::datalayer::VariantType::FLATBUFFERS)
  {
    if (m_dynamic)
    {
      flatbuffers::Offset<comm::datalayer::Reference> vecReferences[] =
      {
          comm::datalayer::CreateReferenceDirect(builder, "readType", m_typeInertialValue.c_str()),
          comm::datalayer::CreateReferenceDirect(builder, "writeType", m_typeInertialValue.c_str()),
      };
      references = builder.CreateVectorOfSortedTables(vecReferences, 2);
    }
    else
    {
      flatbuffers::Offset<comm::datalayer::Reference> vecReferences[] =
      {
          comm::datalayer::CreateReferenceDirect(builder, "readType", m_typeInertialValue.c_str()),
      };
      references = builder.CreateVectorOfSortedTables(vecReferences, 1);
    }
  }

  // Create metadata
  comm::datalayer::MetadataBuilder metadata(builder);
  metadata.add_nodeClass(nodeClass);
  metadata.add_description(description);
  metadata.add_descriptionUrl(description);
  metadata.add_operations(operations);
  if (data.getType() == comm::datalayer::VariantType::FLATBUFFERS)
  {
    metadata.add_references(references);
  }

  auto metaFinished = metadata.Finish();
  builder.Finish(metaFinished);

  comm::datalayer::Variant variant;
  variant.shareFlatbuffers(builder);
  return variant;
}

ProviderNodeAllData::ProviderNodeAllData(comm::datalayer::IProvider3* provider, const std::string& addressRoot, bool dynamic)
  : m_provider(provider)
  , m_addressRoot(addressRoot)
  , m_dynamic(dynamic)
  , m_addressBase(dynamic ? m_addressRoot + "dynamic/" : m_addressRoot + "static/")
{
  m_metadata = createMetadata(comm::datalayer::Variant(), m_addressBase);
}

void ProviderNodeAllData::updateData(int64_t offset100ns, uint64_t edge, uint64_t observer,
                  uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4,
                  int64_t roundtrip100ns)
{
    std::lock_guard<std::mutex> lock(m_dataMutex);

    auto* offsetMsContainer = getDataContainer(m_addressBase + "offset_ms");
    auto* offsetDurationContainer = getDataContainer(m_addressBase + "offset_duration");
    auto* roundtripDurationContainer = getDataContainer(m_addressBase + "RoundTripDelay_duration");
    auto* edgeContainer = getDataContainer(m_addressBase + "edgetimestamp");
    auto* obsContainer = getDataContainer(m_addressBase + "observertimestamp");
    auto* ntp_t1_container = getDataContainer(m_addressBase + "NTP/t1_edge_req_sent");
    auto* ntp_t2_container = getDataContainer(m_addressBase + "NTP/t2_obs_req_arr");
    auto* ntp_t3_container = getDataContainer(m_addressBase + "NTP/t3_obs_reply_sent");
    auto* ntp_t4_container = getDataContainer(m_addressBase + "NTP/t4_edge_reply_arr");
    auto* ntp_offsetDurationContainer = getDataContainer(m_addressBase + "NTP/offset_duration");
    auto* ntp_roundtripDurationContainer = getDataContainer(m_addressBase + "NTP/roundtrip_delay_duration");
    auto* fbContainer = getDataContainer(m_addressBase + "value");

    int64_t offsetMs = offset100ns / 10000;
    int64_t absOffsetMs = offsetMs < 0 ? -offsetMs : offsetMs;
    int64_t hours = absOffsetMs / 3600000;
    int64_t minutes = (absOffsetMs % 3600000) / 60000;
    int64_t seconds = (absOffsetMs % 60000) / 1000;
    int64_t millis = absOffsetMs % 1000;

    std::ostringstream durationStream;
    if (offsetMs < 0) {
        durationStream << "-";
    }
    if (hours > 0) {
        durationStream << hours << "h";
    }
    if (minutes > 0 || hours > 0) {
        durationStream << minutes << "m";
    }
    if (seconds > 0 || minutes > 0 || hours > 0) {
        durationStream << seconds;
        if (millis != 0) {
            durationStream << "." << std::setw(3) << std::setfill('0') << millis;
        }
        durationStream << "s";
    }
    else if (millis != 0) {
        durationStream << millis << "ms";
    }
    else {
        durationStream << "0ms";
    }

    std::string offsetDuration = durationStream.str();

    // --- format roundtrip duration (from roundtrip100ns) ---
    int64_t roundtripMs = roundtrip100ns / 10000;
    int64_t absRoundtripMs = roundtripMs < 0 ? -roundtripMs : roundtripMs;
    int64_t r_hours = absRoundtripMs / 3600000;
    int64_t r_minutes = (absRoundtripMs % 3600000) / 60000;
    int64_t r_seconds = (absRoundtripMs % 60000) / 1000;
    int64_t r_millis = absRoundtripMs % 1000;

    std::ostringstream roundtripStream;
    if (roundtripMs < 0) {
      roundtripStream << "-";
    }
    if (r_hours > 0) {
      roundtripStream << r_hours << "h";
    }
    if (r_minutes > 0 || r_hours > 0) {
      roundtripStream << r_minutes << "m";
    }
    if (r_seconds > 0 || r_minutes > 0 || r_hours > 0) {
      roundtripStream << r_seconds;
      if (r_millis != 0) {
        roundtripStream << "." << std::setw(3) << std::setfill('0') << r_millis;
      }
      roundtripStream << "s";
    }
    else if (r_millis != 0) {
      roundtripStream << r_millis << "ms";
    }
    else {
      roundtripStream << "0ms";
    }

    std::string roundtripDuration = roundtripStream.str();

    // --- update scalars first ---
    if (offsetMsContainer) {
        comm::datalayer::Variant v;
        v.setValue(offsetMs);
        offsetMsContainer->m_data = v;
    }

    if (offsetDurationContainer) {
        comm::datalayer::Variant v;
        v.setValue(offsetDuration);
        offsetDurationContainer->m_data = v;
    }

    if (roundtripDurationContainer) {
        comm::datalayer::Variant v;
        v.setValue(roundtripDuration);
        roundtripDurationContainer->m_data = v;
    }

    // duplicate the offset_duration inside NTP folder if present
    if (ntp_offsetDurationContainer) {
      comm::datalayer::Variant v;
      v.setValue(offsetDuration);
      ntp_offsetDurationContainer->m_data = v;
    }

    if (edgeContainer) {
        comm::datalayer::Variant v;
        v.setTimestamp(edge);
        edgeContainer->m_data = v;
    }

    if (obsContainer) {
        comm::datalayer::Variant v;
        v.setTimestamp(observer);
        obsContainer->m_data = v;
    }

    // NTP timestamp variables
    if (ntp_t1_container) {
      comm::datalayer::Variant v;
      v.setTimestamp(t1);
      ntp_t1_container->m_data = v;
    }
    if (ntp_t2_container) {
      comm::datalayer::Variant v;
      v.setTimestamp(t2);
      ntp_t2_container->m_data = v;
    }
    if (ntp_t3_container) {
      comm::datalayer::Variant v;
      v.setTimestamp(t3);
      ntp_t3_container->m_data = v;
    }
    if (ntp_t4_container) {
      comm::datalayer::Variant v;
      v.setTimestamp(t4);
      ntp_t4_container->m_data = v;
    }

    // roundtrip delay duration
    if (ntp_roundtripDurationContainer) {
      comm::datalayer::Variant v;
      v.setValue(roundtripDuration);
      ntp_roundtripDurationContainer->m_data = v;
    }

    // --- update flatbuffers LAST ---
    if (fbContainer) {
        flatbuffers::FlatBufferBuilder builder;
        auto fb = sample::schema::CreateTimeObserver(builder, offsetMs, edge, roundtripMs);
        builder.Finish(fb);
        fbContainer->m_data.shareFlatbuffers(builder);
    }

    // Note: Notification of changes would be sent here if the API supported it
}


void ProviderNodeAllData::registerNodes()
{
    comm::datalayer::DlResult result;
    comm::datalayer::Variant data;

    // --- Register flatbuffer type first ---
    std::string bfbs = isSnap()
        ? std::string(snapPath()).append("/sampleSchema.bfbs")
        : "bfbs/sampleSchema.bfbs";

    result = m_provider->registerType(m_typeInertialValue, bfbs);
    if (result != comm::datalayer::DlResult::DL_OK)
    {
        std::cout << "ERROR registerType()" << std::endl;
        return;
    }

    // --- Get current system timestamp in FILETIME 100ns units ---
    uint64_t currentTime = getCurrentTimestampFiletime();

    // --- offset in milliseconds ---
    data = comm::datalayer::Variant();
    result = data.setValue((int64_t)0);
    createDataContainer(result, "offset_ms", data);

    // --- RoundTripDelay duration ---
    data = comm::datalayer::Variant();
    result = data.setValue(std::string("0ms"));
    createDataContainer(result, "RoundTripDelay_duration", data);
    
    // --- offset duration ---
    data = comm::datalayer::Variant();
    result = data.setValue(std::string("0ms"));
    createDataContainer(result, "offset_duration", data);

    // --- observer timestamp ---
    data = comm::datalayer::Variant();
    result = data.setTimestamp(currentTime);
    createDataContainer(result, "observertimestamp", data);

    // --- edge timestamp ---
    data = comm::datalayer::Variant();
    result = data.setTimestamp(currentTime);
    createDataContainer(result, "edgetimestamp", data);

    // --- NTP folder variables ---
    // t1_edge_req_sent
    data = comm::datalayer::Variant();
    result = data.setTimestamp(currentTime);
    createDataContainer(result, "NTP/t1_edge_req_sent", data);

    // t2_obs_req_arr
    data = comm::datalayer::Variant();
    result = data.setTimestamp(currentTime);
    createDataContainer(result, "NTP/t2_obs_req_arr", data);

    // t3_obs_reply_sent
    data = comm::datalayer::Variant();
    result = data.setTimestamp(currentTime);
    createDataContainer(result, "NTP/t3_obs_reply_sent", data);

    // t4_edge_reply_arr
    data = comm::datalayer::Variant();
    result = data.setTimestamp(currentTime);
    createDataContainer(result, "NTP/t4_edge_reply_arr", data);

    // duplicate offset_duration inside NTP
    data = comm::datalayer::Variant();
    result = data.setValue(std::string("0ms"));
    createDataContainer(result, "NTP/offset_duration", data);

    // roundtrip delay duration
    data = comm::datalayer::Variant();
    result = data.setValue(std::string("0ms"));
    createDataContainer(result, "NTP/roundtrip_delay_duration", data);

    // --- flatbuffer TimeObserver ---
    flatbuffers::FlatBufferBuilder builder;
    int64_t offset = 0;
    uint64_t edge = currentTime;
    int64_t roundTripDelay = 0;

    auto fb = sample::schema::CreateTimeObserver(builder, offset, edge, roundTripDelay);
    builder.Finish(fb);

    data = comm::datalayer::Variant();
    result = data.shareFlatbuffers(builder);
    createDataContainer(result, "value", data);
}
// This function will be called whenever a object should be created.
void ProviderNodeAllData::onCreate(
  const std::string& address,
  const comm::datalayer::Variant* data,
  const comm::datalayer::IProviderNode::ResponseCallback& callback)
{
  comm::datalayer::DlResult result;

  if (m_dynamic == false)
  {
    result = comm::datalayer::DlResult::DL_PERMISSION_DENIED;
    std::cout << "onCreate(): " << address << " " << result.toString() << std::endl;
    callback(result, nullptr);
    return;
  }

  static comm::datalayer::Variant empty;
  comm::datalayer::Variant myData = data != nullptr ? *data : empty; // Avoid nullptr assignment

  result = createDataContainer(address, myData);
  std::cout << "onCreate(): " << address << " " << result.toString() << std::endl;

  callback(result, nullptr);
}

// Read function of a node. Function will be called whenever a node should be read.
void ProviderNodeAllData::onRead(
  const std::string& address,
  const comm::datalayer::Variant* data,
  const comm::datalayer::IProviderNode::ResponseCallback& callback)
{
  std::lock_guard<std::mutex> lock(m_dataMutex);

  auto dataContainer = getDataContainer(address);
  if (nullptr == dataContainer)
  {
    callback(comm::datalayer::DlResult::DL_INVALID_ADDRESS, nullptr);
    return;
  }

  auto result = dataContainer->m_errorCode >= 0
        ? comm::datalayer::DlResult::DL_OK
        : comm::datalayer::DlResult::DL_INVALID_VALUE;

  //  FIXED: no temporary
  callback(result, &dataContainer->m_data);
}


// Write function of a node. Function will be called whenever a node should be written.
void ProviderNodeAllData::onWrite(
  const std::string& address,
  const comm::datalayer::Variant* data,
  const comm::datalayer::IProviderNode::ResponseCallback& callback)
{
  comm::datalayer::DlResult result;

  if (m_dynamic == false)
  {
    result = comm::datalayer::DlResult::DL_PERMISSION_DENIED;
    std::cout << "onWrite(): " << address << " ";
    std::cout << data;
    std::cout << " " << result.toString() << std::endl;
    callback(result, nullptr);
    return;
  }

  DataContainer* dataContainer = getDataContainer(address);
  if (nullptr == dataContainer)
  {
    // A virtual node which is not registered should be written.
    // We a creating and registering the virtual node now. This makes ctrlX Data Layer usage easy for a client.
    result = createDataContainer(address, *data);
    std::cout << "onWrite() CREATE: " << address << " " << result.toString() << std::endl;
  }
  else
  {
    // The virtual node is already registered.
    result = comm::datalayer::DlResult::DL_OK;
    // We are take the data without data type check.
    dataContainer->m_data = *data;
    // No trace to improve performance
    // std::cout << "onWrite(): " << address << " OK" << std::endl;
  }

  callback(result, data);
}

// Remove function for an object. Function will be called whenever a object should be removed.
void ProviderNodeAllData::onRemove(
  const std::string& address,
  const comm::datalayer::IProviderNode::ResponseCallback& callback)
{
  comm::datalayer::DlResult result;

  if (m_dynamic == false)
  {
    result = comm::datalayer::DlResult::DL_PERMISSION_DENIED;
    std::cout << "onRemove(): " << address << " " << result.toString() << std::endl;
    callback(result, nullptr);
    return;
  }

  DataContainer* dataContainer = getDataContainer(address);
  if (nullptr != dataContainer)
  {
    m_provider->unregisterNode(dataContainer->m_address);
    std::remove(m_dataContainers.begin(), m_dataContainers.end(), dataContainer);
    delete dataContainer;
  }

  result = comm::datalayer::DlResult::DL_OK;
  std::cout << "onRemove(): " << address << " " << result.toString() << std::endl;
  callback(result, nullptr);
}

// Browse function of a node. Function will be called to determine children of a node.
void ProviderNodeAllData::onBrowse(
  const std::string& address,
  const comm::datalayer::IProviderNode::ResponseCallback& callback)
{
  // No trace to improve performance
  //std::cout << "onBrowse(): " << address << std::endl;

  callback(comm::datalayer::DlResult::DL_OK, nullptr); // ctrlX Data Layer Broker "knows" all sub node, we don't add further nodes here.
}

// Read function of metadata of an object. Function will be called whenever the metadata of a node should be written.
void ProviderNodeAllData::onMetadata(
  const std::string& address,
  const comm::datalayer::IProviderNode::ResponseCallback& callback)
{
  comm::datalayer::DlResult result;
  DataContainer* dataContainer = getDataContainer(address); // For .../dynamic or .../static we get also nullptr
  if (nullptr == dataContainer)
  {
    if (address + "/" == m_addressBase)
    {
      callback(comm::datalayer::DlResult::DL_OK, &m_metadata);
      return;
    }
    result = comm::datalayer::DlResult::DL_INVALID_ADDRESS;
    std::cout << "onMetadata(): " << address << " " << result.toString() << std::endl;
    callback(result, nullptr);
    return;
  }
  callback(comm::datalayer::DlResult::DL_OK, &dataContainer->m_metadata);
}
