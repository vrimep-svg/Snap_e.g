/*
 * SPDX-FileCopyrightText: Bosch Rexroth AG
 *
 * SPDX-License-Identifier: MIT
 */

#include <string>

#include "providerNodeAllData.h"
#include "sampleSchema_generated.h"
#include "ctrlx_datalayer_helper.h"

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

void ProviderNodeAllData::updateData(uint64_t offset, uint64_t edge, uint64_t observer)
{
	
    std::lock_guard<std::mutex> lock(m_dataMutex);

	auto* offsetContainer = getDataContainer(m_addressBase + "offset");
	auto* edgeContainer = getDataContainer(m_addressBase + "edgetimestamp");
	auto* obsContainer = getDataContainer(m_addressBase + "observertimestamp");
	auto* fbContainer = getDataContainer(m_addressBase + "value");

	// --- update scalars first ---
	if (offsetContainer) {
		comm::datalayer::Variant v;
		v.setValue(offset);
		offsetContainer->m_data = v;
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

	// --- update flatbuffer LAST ---
	if (fbContainer) {
		flatbuffers::FlatBufferBuilder builder;
		auto fb = sample::schema::CreateTimeObserver(builder, offset, edge, observer);
		builder.Finish(fb);
		fbContainer->m_data.shareFlatbuffers(builder);
	}
	
	//Only notify if container exists:
	if (offsetContainer)
		m_provider->notifyNodeChanged(m_addressBase + "offset");

	if (edgeContainer)
		m_provider->notifyNodeChanged(m_addressBase + "edgetimestamp");

	if (obsContainer)
		m_provider->notifyNodeChanged(m_addressBase + "observertimestamp");

	if (fbContainer)
		m_provider->notifyNodeChanged(m_addressBase + "value");
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

    // --- offset ---
    data.reset();
    result = data.setValue((uint64_t)0);
    createDataContainer(result, "offset", data);

    // --- observer timestamp ---
    data.reset();
	uint64_t initialTs = 116444736000000000ULL;
    result = data.setTimestamp(initialTs);
    createDataContainer(result, "observertimestamp", data);

    // --- edge timestamp ---
    data.reset();
    result = data.setTimestamp(initialTs);
    createDataContainer(result, "edgetimestamp", data);

    // --- flatbuffer ---
    flatbuffers::FlatBufferBuilder builder;

    uint64_t offset = 1;
    uint64_t edge = 2;
    uint64_t observer = 3;

    auto fb = sample::schema::CreateTimeObserver(builder, offset, edge, observer);
    builder.Finish(fb);

    data.reset();
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
