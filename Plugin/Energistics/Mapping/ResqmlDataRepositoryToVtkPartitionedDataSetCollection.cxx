/*-----------------------------------------------------------------------
Licensed to the Apache Software Foundation (ASF) under one
or more contributor license agreements.  See the NOTICE file
distributed with this work for additional information
regarding copyright ownership.  The ASF licenses this file
to you under the Apache License, Version 2.0 (the
"License"; you may not use this file except in compliance
with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied.  See the License for the
specific language governing permissions and limitations
under the License.
-----------------------------------------------------------------------*/

/*
Documentation:
==============

VtkAssembly => TreeView:
    - each node required 2 attributes:
        - id = uuid of resqml/witsml object
        - label = name to display in TreeView
        - type = type
*/
#include "ResqmlDataRepositoryToVtkPartitionedDataSetCollection.h"

#include <algorithm>
#include <vector>
#include <set>
#include <list>
#include <regex>
#include <numeric>
#include <cstdlib>
#include <iostream>

// VTK includes
#include <vtkPartitionedDataSetCollection.h>
#include <vtkPartitionedDataSet.h>
#include <vtkInformation.h>
#include <vtkDataAssembly.h>
#include <vtkDataArraySelection.h>

// FESAPI includes
#include <fesapi/eml2/TimeSeries.h>
#include <fesapi/resqml2/Grid2dRepresentation.h>
#include <fesapi/resqml2/AbstractFeatureInterpretation.h>
#include <fesapi/resqml2/AbstractIjkGridRepresentation.h>
#include <fesapi/resqml2/PolylineSetRepresentation.h>
#include <fesapi/resqml2/SubRepresentation.h>
#include <fesapi/resqml2/TriangulatedSetRepresentation.h>
#include <fesapi/resqml2/UnstructuredGridRepresentation.h>
#include <fesapi/resqml2/WellboreMarkerFrameRepresentation.h>
#include <fesapi/resqml2/WellboreMarker.h>
#include <fesapi/resqml2/WellboreTrajectoryRepresentation.h>
#include <fesapi/resqml2/AbstractFeatureInterpretation.h>
#include <fesapi/resqml2/ContinuousProperty.h>
#include <fesapi/resqml2/DiscreteProperty.h>
#include <fesapi/resqml2/WellboreFeature.h>
#include <fesapi/resqml2/RepresentationSetRepresentation.h>
#include <fesapi/witsml2_1/WellboreCompletion.h>
#include <fesapi/witsml2_1/WellCompletion.h>
#include <fesapi/witsml2_1/Well.h>

#ifdef WITH_ETP_SSL
#include <thread>

#include <fetpapi/etp/fesapi/FesapiHdfProxy.h>

#include <fetpapi/etp/ProtocolHandlers/DataspaceHandlers.h>
#include <fetpapi/etp/ProtocolHandlers/DiscoveryHandlers.h>
#include <fetpapi/etp/ProtocolHandlers/StoreHandlers.h>
#include <fetpapi/etp/ProtocolHandlers/DataArrayHandlers.h>
#endif
#include <fesapi/common/EpcDocument.h>

#include "Mapping/ResqmlIjkGridToVtkExplicitStructuredGrid.h"
#include "Mapping/ResqmlIjkGridSubRepToVtkExplicitStructuredGrid.h"
#include "Mapping/ResqmlGrid2dToVtkStructuredGrid.h"
#include "Mapping/ResqmlPolylineToVtkPolyData.h"
#include "Mapping/ResqmlTriangulatedSetToVtkPartitionedDataSet.h"
#include "Mapping/ResqmlUnstructuredGridToVtkUnstructuredGrid.h"
#include "Mapping/ResqmlUnstructuredGridSubRepToVtkUnstructuredGrid.h"
#include "Mapping/ResqmlWellboreTrajectoryToVtkPolyData.h"
#include "Mapping/ResqmlWellboreMarkerFrameToVtkPartitionedDataSet.h"
#include "Mapping/ResqmlWellboreFrameToVtkPartitionedDataSet.h"
#include "Mapping/WitsmlWellboreCompletionToVtkPartitionedDataSet.h"
#include "Mapping/WitsmlWellboreCompletionPerforationToVtkPolyData.h"
#include "Mapping/CommonAbstractObjectSetToVtkPartitionedDataSetSet.h"

ResqmlDataRepositoryToVtkPartitionedDataSetCollection::ResqmlDataRepositoryToVtkPartitionedDataSetCollection() : markerOrientation(false),
                                                                                                                 markerSize(10),
                                                                                                                 repository(new common::DataObjectRepository()),
                                                                                                                 _output(vtkSmartPointer<vtkPartitionedDataSetCollection>::New()),
                                                                                                                 _nodeIdToMapper(),
                                                                                                                 _currentSelection(),
                                                                                                                 _oldSelection()
{
    auto assembly = vtkSmartPointer<vtkDataAssembly>::New();
    assembly->SetRootNodeName("data");

    _output->SetDataAssembly(assembly);
    times_step.clear();
}

ResqmlDataRepositoryToVtkPartitionedDataSetCollection::~ResqmlDataRepositoryToVtkPartitionedDataSetCollection()
{
    delete repository;
    for (const auto &keyVal : _nodeIdToMapper)
    {
        delete keyVal.second;
    }
}

// This function replaces the VTK function this->MakeValidNodeName(),
// which has a bug in the sorted_valid_chars array. The '.' character is placed
// before the '-' character, which is incorrect. This function uses a valid_chars
// array that correctly sorts the characters. The function checks if each character
// in the input string is valid, and adds it to the output string if it is valid.
// If the first character of the output string is not valid, an underscore is added
// to the beginning of the output string. This function is designed to create a valid
// node name from a given string.
std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::MakeValidNodeName(const char *name)
{
    if (name == nullptr || name[0] == '\0')
    {
        return std::string();
    }

    const char sorted_valid_chars[] =
        "-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
    const auto sorted_valid_chars_len = strlen(sorted_valid_chars);

    std::string result;
    result.reserve(strlen(name));
    for (size_t cc = 0, max = strlen(name); cc < max; ++cc)
    {
        if (std::binary_search(
                sorted_valid_chars, sorted_valid_chars + sorted_valid_chars_len, name[cc]))
        {
            result += name[cc];
        }
    }

    if (result.empty() ||
        ((result[0] < 'a' || result[0] > 'z') && (result[0] < 'A' || result[0] > 'Z') &&
         result[0] != '_'))
    {
        return "_" + result;
    }
    return result;
}

std::string SimplifyXmlTag(std::string type_representation)
{
    std::string suffix = "Representation";
    std::string prefix = "Wellbore";

    if (type_representation.size() >= suffix.size() && type_representation.substr(type_representation.size() - suffix.size()) == suffix)
    {
        type_representation = type_representation.substr(0, type_representation.size() - suffix.size());
    }

    if (type_representation.size() >= prefix.size() && type_representation.substr(0, prefix.size()) == prefix)
    {
        type_representation = type_representation.substr(prefix.size());
    }
    return type_representation;
}

//----------------------------------------------------------------------------
std::vector<std::string> ResqmlDataRepositoryToVtkPartitionedDataSetCollection::connect(const std::string &etp_url, const std::string &data_partition, const std::string &auth_connection)
{
    std::vector<std::string> result;
#ifdef WITH_ETP_SSL
    boost::uuids::random_generator gen;
    ETP_NS::InitializationParameters initializationParams(gen(), etp_url);

    std::map<std::string, std::string> additionalHandshakeHeaderFields = {{"data-partition-id", data_partition}};
    if (etp_url.find("ws://") == 0)
    {
        session = ETP_NS::ClientSessionLaunchers::createWsClientSession(&initializationParams, auth_connection, additionalHandshakeHeaderFields);
    }
    else
    {
        session = ETP_NS::ClientSessionLaunchers::createWssClientSession(&initializationParams, auth_connection, additionalHandshakeHeaderFields);
    }
    try
    {
        session->setDataspaceProtocolHandlers(std::make_shared<ETP_NS::DataspaceHandlers>(session.get()));
    }
    catch (const std::exception &e)
    {
        vtkOutputWindowDisplayErrorText((std::string("fesapi error > ") + e.what()).c_str());
    }
    try
    {
        session->setDiscoveryProtocolHandlers(std::make_shared<ETP_NS::DiscoveryHandlers>(session.get()));
    }
    catch (const std::exception &e)
    {
        vtkOutputWindowDisplayErrorText((std::string("fesapi error > ") + e.what()).c_str());
    }
    try
    {
        session->setStoreProtocolHandlers(std::make_shared<ETP_NS::StoreHandlers>(session.get()));
    }
    catch (const std::exception &e)
    {
        vtkOutputWindowDisplayErrorText((std::string("fesapi error > ") + e.what()).c_str());
    }
    try
    {
        session->setDataArrayProtocolHandlers(std::make_shared<ETP_NS::DataArrayHandlers>(session.get()));
    }
    catch (const std::exception &e)
    {
        vtkOutputWindowDisplayErrorText((std::string("fesapi error > ") + e.what()).c_str());
    }

    repository->setHdfProxyFactory(new ETP_NS::FesapiHdfProxyFactory(session.get()));

    //
    if (etp_url.find("ws://") == 0)
    {
        auto plainSession = std::dynamic_pointer_cast<ETP_NS::PlainClientSession>(session);
        std::thread sessionThread(&ETP_NS::PlainClientSession::run, plainSession);
        sessionThread.detach();
    }
    else
    {
        auto sslSession = std::dynamic_pointer_cast<ETP_NS::SslClientSession>(session);
        std::thread sessionThread(&ETP_NS::SslClientSession::run, sslSession);
        sessionThread.detach();
    }

    // Wait for the ETP session to be opened
    auto t_start = std::chrono::high_resolution_clock::now();
    while (session->isEtpSessionClosed())
    {
        if (std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t_start).count() > 5000)
        {
            throw std::invalid_argument("Did you forget to click apply button before to connect? Time out for websocket connection" +
                                        std::to_string(std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t_start).count()) + "ms.\n");
        }
    }

    //************ LIST DATASPACES ************
    const auto dataspaces = session->getDataspaces();

    std::transform(dataspaces.begin(), dataspaces.end(), std::back_inserter(result),
                   [](const Energistics::Etp::v12::Datatypes::Object::Dataspace &ds)
                   { return ds.uri; });

#endif
    return result;
}

//----------------------------------------------------------------------------
void ResqmlDataRepositoryToVtkPartitionedDataSetCollection::disconnect()
{
#ifdef WITH_ETP_SSL
    session->close();
#endif
}

//----------------------------------------------------------------------------
std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::addFile(const char *fileName)
{
    COMMON_NS::EpcDocument pck(fileName);
    std::string message = pck.deserializeInto(*repository);
    pck.close();

    message += buildDataAssemblyFromDataObjectRepo(fileName);
    return message;
}

//----------------------------------------------------------------------------
std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::addDataspace(const char *dataspace)
{
#ifdef WITH_ETP_SSL
    //************ LIST RESOURCES ************
    Energistics::Etp::v12::Datatypes::Object::ContextInfo ctxInfo;
    ctxInfo.uri = dataspace;
    ctxInfo.depth = 0;
    ctxInfo.navigableEdges = Energistics::Etp::v12::Datatypes::Object::RelationshipKind::Both;
    ctxInfo.includeSecondaryTargets = false;
    ctxInfo.includeSecondarySources = false;
    const auto resources = session->getResources(ctxInfo, Energistics::Etp::v12::Datatypes::Object::ContextScopeKind::targets);

    //************ GET ALL DATAOBJECTS ************
    repository->setHdfProxyFactory(new ETP_NS::FesapiHdfProxyFactory(session.get()));
    if (!resources.empty())
    {
        std::map<std::string, std::string> query;
        for (size_t i = 0; i < resources.size(); ++i)
        {
            query[std::to_string(i)] = resources[i].uri;
        }
        const auto dataobjects = session->getDataObjects(query);
        for (auto &datoObject : dataobjects)
        {
            repository->addOrReplaceGsoapProxy(datoObject.second.data,
                                               ETP_NS::EtpHelpers::getDataObjectType(datoObject.second.resource.uri),
                                               ETP_NS::EtpHelpers::getDataspaceUri(datoObject.second.resource.uri));
        }
    }
    else
    {
        vtkOutputWindowDisplayWarningText(("There is no dataobject in the dataspace : " + std::string(dataspace) + "\n").c_str());
    }
#endif
    return buildDataAssemblyFromDataObjectRepo("");
}

namespace
{
    auto lexicographicalComparison = [](const COMMON_NS::AbstractObject *a, const COMMON_NS::AbstractObject *b) -> bool
    {
        return a->getTitle().compare(b->getTitle()) < 0;
    };

    template <typename T>
    void sortAndAdd(std::vector<T> source, std::vector<RESQML2_NS::AbstractRepresentation const *> &dest)
    {
        std::sort(source.begin(), source.end(), lexicographicalComparison);
        std::move(source.begin(), source.end(), std::inserter(dest, dest.end()));
    }
}

std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::buildDataAssemblyFromDataObjectRepo(const char *fileName)
{
    std::vector<RESQML2_NS::AbstractRepresentation const *> allReps;

    // create vtkDataAssembly: create treeView in property panel
    sortAndAdd(repository->getHorizonGrid2dRepresentationSet(), allReps);
    sortAndAdd(repository->getIjkGridRepresentationSet(), allReps);
    sortAndAdd(repository->getAllPolylineSetRepresentationSet(), allReps);
    sortAndAdd(repository->getAllTriangulatedSetRepresentationSet(), allReps);
    sortAndAdd(repository->getUnstructuredGridRepresentationSet(), allReps);

    // See https://stackoverflow.com/questions/15347123/how-to-construct-a-stdstring-from-a-stdvectorstring
    std::string message = std::accumulate(std::begin(allReps), std::end(allReps), std::string{},
                                          [&](std::string &message, RESQML2_NS::AbstractRepresentation const *rep)
                                          {
                                              return message += searchRepresentations(rep);
                                          });
    // get WellboreTrajectory
    message += searchWellboreTrajectory(fileName);

    // get TimeSeries
    message += searchTimeSeries(fileName);

    return message;
}

std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::searchRepresentations(resqml2::AbstractRepresentation const *representation, int idNode)
{
    std::string result;
    vtkDataAssembly *data_assembly = _output->GetDataAssembly();

    if (representation->isPartial())
    {
        // check if it has already been added
        // not exist => not loaded
        if (data_assembly->FindFirstNodeWithName(("_" + representation->getUuid()).c_str()) == -1)
        {
            return "Partial representation with UUID \"" + representation->getUuid() + "\" is not loaded.\n";
        } /******* TODO ********/ // exist but not the same type ?
    }
    else
    {
        // The leading underscore is forced by VTK which does not support a node name starting with a digit (probably because it is a QNAME).
        const std::string nodeName = "_" + representation->getUuid();
        const int existingNodeId = _output->GetDataAssembly()->FindFirstNodeWithName(nodeName.c_str());
        if (existingNodeId == -1)
        {
            idNode = data_assembly->AddNode(nodeName.c_str(), idNode);

            auto const *subrep = dynamic_cast<RESQML2_NS::SubRepresentation const *>(representation);
            // To shorten the xmlTag by removing �Representation� from the end.

            std::string type_representation = SimplifyXmlTag(representation->getXmlTag());

            const std::string representationVtkValidName = subrep == nullptr
                                                               ? this->MakeValidNodeName((type_representation + "_" + representation->getTitle()).c_str())
                                                               : this->MakeValidNodeName((type_representation + "_" + subrep->getSupportingRepresentation(0)->getTitle() + "_" + representation->getTitle()).c_str());

            const TreeViewNodeType w_type = subrep == nullptr
                                                ? TreeViewNodeType::Representation
                                                : TreeViewNodeType::SubRepresentation;

            data_assembly->SetAttribute(idNode, "label", representationVtkValidName.c_str());
            data_assembly->SetAttribute(idNode, "type", std::to_string(static_cast<int>(TreeViewNodeType::Representation)).c_str());
        }
        else
        {
            idNode = existingNodeId;
        }
    }

    // add sub representation with properties (only for ijkGrid and unstructured grid)
    if (dynamic_cast<RESQML2_NS::AbstractIjkGridRepresentation const *>(representation) != nullptr ||
        dynamic_cast<RESQML2_NS::UnstructuredGridRepresentation const *>(representation) != nullptr)
    {
        result += searchSubRepresentation(representation, data_assembly, idNode);
    }

    // add properties to representation
    result += searchProperties(representation, data_assembly, idNode);

    return result;
}

std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::searchSubRepresentation(RESQML2_NS::AbstractRepresentation const *representation, vtkDataAssembly *data_assembly, int node_parent)
{
    try
    {
        auto subRepresentationSet = representation->getSubRepresentationSet();
        std::sort(subRepresentationSet.begin(), subRepresentationSet.end(), lexicographicalComparison);

        std::string message = std::accumulate(std::begin(subRepresentationSet), std::end(subRepresentationSet), std::string{},
                                              [&](std::string &message, RESQML2_NS::SubRepresentation *b)
                                              {
                                                  return message += searchRepresentations(b, data_assembly->GetParent(node_parent));
                                              });
    }
    catch (const std::exception &e)
    {
        return "Exception in FESAPI when calling getSubRepresentationSet for uuid : " + representation->getUuid() + " : " + e.what() + ".\n";
    }

    return "";
}

std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::searchProperties(RESQML2_NS::AbstractRepresentation const *representation, vtkDataAssembly *data_assembly, int node_parent)
{
    try
    {
        auto valuesPropertySet = representation->getValuesPropertySet();
        std::sort(valuesPropertySet.begin(), valuesPropertySet.end(), lexicographicalComparison);
        // property
        for (auto const *property : valuesPropertySet)
        {
            const std::string propertyVtkValidName = this->MakeValidNodeName((property->getXmlTag() + '_' + property->getTitle()).c_str());

            if (_output->GetDataAssembly()->FindFirstNodeWithName(("_" + property->getUuid()).c_str()) == -1)
            { // verify uuid exist in treeview
                int property_idNode = data_assembly->AddNode(("_" + property->getUuid()).c_str(), node_parent);
                data_assembly->SetAttribute(property_idNode, "label", propertyVtkValidName.c_str());
                data_assembly->SetAttribute(property_idNode, "type", std::to_string(static_cast<int>(TreeViewNodeType::Properties)).c_str());
            }
        }
    }
    catch (const std::exception &e)
    {
        return "Exception in FESAPI when calling getValuesPropertySet with representation uuid: " + representation->getUuid() + " : " + e.what() + ".\n";
    }

    return "";
}

int ResqmlDataRepositoryToVtkPartitionedDataSetCollection::searchRepresentationSetRepresentation(resqml2::RepresentationSetRepresentation const* rsr, int idNode)
{
    vtkDataAssembly* data_assembly = _output->GetDataAssembly();

    if (_output->GetDataAssembly()->FindFirstNodeWithName(("_" + rsr->getUuid()).c_str()) == -1)
    { // verify uuid exist in treeview
      // To shorten the xmlTag by removing �Representation� from the end.
        for (resqml2::RepresentationSetRepresentation* rsr : rsr->getRepresentationSetRepresentationSet())
        {
            idNode = searchRepresentationSetRepresentation(rsr, idNode);
        }
        if (_output->GetDataAssembly()->FindFirstNodeWithName(("_" + rsr->getUuid()).c_str()) == -1)
        {
            const std::string rsrVtkValidName = this->MakeValidNodeName(("Collection_"+rsr->getTitle()).c_str());
            idNode = data_assembly->AddNode(("_" + rsr->getUuid()).c_str(), idNode);
            data_assembly->SetAttribute(idNode, "label", rsrVtkValidName.c_str());
            data_assembly->SetAttribute(idNode, "type", std::to_string(static_cast<int>(TreeViewNodeType::Collection)).c_str());
        }
    }
    else
    {
        return _output->GetDataAssembly()->FindFirstNodeWithName(("_" + rsr->getUuid()).c_str());
    }
    return idNode;
}

std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::searchWellboreTrajectory(const std::string &fileName)
{
    std::string result;

    vtkDataAssembly *data_assembly = _output->GetDataAssembly();

    int idNode = 0;
    for (auto *wellboreTrajectory : repository->getWellboreTrajectoryRepresentationSet())
    {
        int idNode = 0;
        int idNodeInit = 0;
        if (_output->GetDataAssembly()->FindFirstNodeWithName(("_" + wellboreTrajectory->getUuid()).c_str()) == -1)
        { // verify uuid exist in treeview
          // To shorten the xmlTag by removing �Representation� from the end.

            for (resqml2::RepresentationSetRepresentation* rsr : wellboreTrajectory->getRepresentationSetRepresentationSet())
            {
                idNodeInit = searchRepresentationSetRepresentation(rsr);
            }

            auto* wellbore = wellboreTrajectory->getInterpretation()->getInterpretedFeature();
            if (_output->GetDataAssembly()->FindFirstNodeWithName(("_" + wellbore->getUuid()).c_str()) == -1)
            {
                vtkOutputWindowDisplayDebugText(wellbore->getTitle().c_str());
                const std::string wellboreVtkValidName = "Wellbore_"+this->MakeValidNodeName(wellbore->getTitle().c_str());
                vtkOutputWindowDisplayDebugText(wellboreVtkValidName.c_str());
                idNodeInit = data_assembly->AddNode(("_" + wellbore->getUuid()).c_str(), idNodeInit);
                data_assembly->SetAttribute(idNodeInit, "label", wellboreVtkValidName.c_str());
                data_assembly->SetAttribute(idNodeInit, "type", std::to_string(static_cast<int>(TreeViewNodeType::Wellbore)).c_str());
            }

            if (wellboreTrajectory->isPartial())
            {
                // check if it has already been added

                const std::string name_partial_representation = this->MakeValidNodeName((SimplifyXmlTag(wellboreTrajectory->getXmlTag()) + "_" + wellboreTrajectory->getTitle()).c_str());
                bool uuid_exist = data_assembly->FindFirstNodeWithName(("_" + name_partial_representation).c_str()) != -1;
                // not exist => not loaded
                if (!uuid_exist)
                {
                    result = result + " Partial UUID: (" + wellboreTrajectory->getUuid() + ") is not loaded \n";
                    continue;
                } /******* TODO ********/ // exist but not the same type ?
            }
            else
            {
                const std::string wellboreTrajectoryVtkValidName = this->MakeValidNodeName((SimplifyXmlTag(wellboreTrajectory->getXmlTag()) + '_' + wellboreTrajectory->getTitle()).c_str());
                idNode = data_assembly->AddNode(("_" + wellboreTrajectory->getUuid()).c_str(), idNodeInit);
                data_assembly->SetAttribute(idNode, "label", wellboreTrajectoryVtkValidName.c_str());
                data_assembly->SetAttribute(idNode, "type", std::to_string(static_cast<int>(TreeViewNodeType::WellboreTrajectory)).c_str());

            }
        }
        // wellboreFrame
        for (auto *wellboreFrame : wellboreTrajectory->getWellboreFrameRepresentationSet())
        {
            if (_output->GetDataAssembly()->FindFirstNodeWithName(("_" + wellboreFrame->getUuid()).c_str()) == -1)
            { // verify uuid exist in treeview
                auto *wellboreMarkerFrame = dynamic_cast<RESQML2_NS::WellboreMarkerFrameRepresentation const *>(wellboreFrame);
                if (wellboreMarkerFrame == nullptr)
                { // WellboreFrame
                    //wellboreFrame->setTitle(this->MakeValidNodeName((wellboreFrame->getXmlTag() + '_' + wellboreFrame->getTitle()).c_str()));
                    const std::string wellboreFrameVtkValidName = this->MakeValidNodeName((SimplifyXmlTag(wellboreFrame->getXmlTag()) + '_' + wellboreFrame->getTitle()).c_str());
                    int frame_idNode = data_assembly->AddNode(("_" + wellboreFrame->getUuid()).c_str(), idNodeInit);
                    data_assembly->SetAttribute(frame_idNode, "label", wellboreFrameVtkValidName.c_str());
                    data_assembly->SetAttribute(frame_idNode, "traj", idNode);
                    data_assembly->SetAttribute(frame_idNode, "type", std::to_string(static_cast<int>(TreeViewNodeType::WellboreFrame)).c_str());
                    for (auto *property : wellboreFrame->getValuesPropertySet())
                    {
                        const std::string propertyVtkValidName = this->MakeValidNodeName((property->getXmlTag() + '_' + property->getTitle()).c_str());
                        int property_idNode = data_assembly->AddNode(("_" + property->getUuid()).c_str(), frame_idNode);
                        data_assembly->SetAttribute(property_idNode, "label", propertyVtkValidName.c_str());
                        data_assembly->SetAttribute(property_idNode, "traj", idNode);
                        data_assembly->SetAttribute(property_idNode, "type", std::to_string(static_cast<int>(TreeViewNodeType::WellboreChannel)).c_str());
                    }
                }
                else
                { // WellboreMarkerFrame
                    const std::string wellboreFrameVtkValidName = this->MakeValidNodeName((SimplifyXmlTag(wellboreFrame->getXmlTag()) + '_' + wellboreFrame->getTitle()).c_str());
                    int markerFrame_idNode = data_assembly->AddNode(("_" + wellboreFrame->getUuid()).c_str(), idNodeInit);
                    data_assembly->SetAttribute(markerFrame_idNode, "label", wellboreFrameVtkValidName.c_str());
                    data_assembly->SetAttribute(markerFrame_idNode, "traj", idNode);
                    data_assembly->SetAttribute(markerFrame_idNode, "type", std::to_string(static_cast<int>(TreeViewNodeType::WellboreMarkerFrame)).c_str());
                    for (auto *wellboreMarker : wellboreMarkerFrame->getWellboreMarkerSet())
                    {
                        const std::string wellboreMarkerVtkValidName = this->MakeValidNodeName((wellboreMarker->getXmlTag() + '_' + wellboreMarker->getTitle()).c_str());
                        int marker_idNode = data_assembly->AddNode(("_" + wellboreMarker->getUuid()).c_str(), markerFrame_idNode);
                        data_assembly->SetAttribute(marker_idNode, "label", wellboreMarkerVtkValidName.c_str());
                        data_assembly->SetAttribute(marker_idNode, "traj", idNode);
                        data_assembly->SetAttribute(marker_idNode, "type", std::to_string(static_cast<int>(TreeViewNodeType::WellboreMarker)).c_str());
                    }
                }
            }
        }
        // Wellbore Completion
        const auto *wellboreFeature = dynamic_cast<RESQML2_NS::WellboreFeature *>(wellboreTrajectory->getInterpretation()->getInterpretedFeature());
        if (wellboreFeature != nullptr)
        {
            witsml2::Wellbore *witsmlWellbore = nullptr;
            if (const auto *witsmlWellbore = dynamic_cast<witsml2::Wellbore *>(wellboreFeature->getWitsmlWellbore()))
            {
                /*
                witsml2::Well* well = witsmlWellbore->getWell();
                gsoap_eml2_3::witsml21__WellFluid fluid = well->getFluidWell();
                gsoap_eml2_3::witsml21__WellDirection direction = well->getDirectionWell();
                WellboreStatut statut;
                if (fluid == gsoap_eml2_3::witsml21__WellFluid::oil) {
                    if (direction == gsoap_eml2_3::witsml21__WellDirection::injector) {
                        statut = WellboreStatut::OilInjecter;
                    } else if (direction == gsoap_eml2_3::witsml21__WellDirection::producer) {
                        statut = WellboreStatut::OilProducer;
                    }
                }
                else if (fluid == gsoap_eml2_3::witsml21__WellFluid::water) {
                    if (direction == gsoap_eml2_3::witsml21__WellDirection::injector) {
                        statut = WellboreStatut::WaterInjecter;
                    }
                    else if (direction == gsoap_eml2_3::witsml21__WellDirection::producer) {
                        statut = WellboreStatut::WaterProducer;
                    }
                }
                else if (fluid == gsoap_eml2_3::witsml21__WellFluid::gas) {
                    if (direction == gsoap_eml2_3::witsml21__WellDirection::injector) {
                        statut = WellboreStatut::GazInjecter;
                    }
                    else if (direction == gsoap_eml2_3::witsml21__WellDirection::producer) {
                        statut = WellboreStatut::GazProducer;
                    }
                }
                */
                for (const auto *wellbore_completion : witsmlWellbore->getWellboreCompletionSet())
                {
                    const std::string wellboreCompletionVtkValidName = this->MakeValidNodeName((SimplifyXmlTag(wellbore_completion->getXmlTag()) + '_' + wellbore_completion->getTitle()).c_str());
                    int idNode_completion = data_assembly->AddNode(("_" + wellbore_completion->getUuid()).c_str(), idNodeInit);
                    data_assembly->SetAttribute(idNode_completion, "label", wellboreCompletionVtkValidName.c_str());
                    data_assembly->SetAttribute(idNode_completion, "type", std::to_string(static_cast<int>(TreeViewNodeType::WellboreCompletion)).c_str());
                    // Iterate over the perforations.
                    for (uint64_t perforationIndex = 0; perforationIndex < wellbore_completion->getConnectionCount(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION); ++perforationIndex)
                    {
                        std::string perforation_name = "Perfo";
                        std::string perforation_skin = "";
                        std::string perforation_diameter = "";

                        auto search_perforation_name = wellbore_completion->getConnectionExtraMetadata(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION, perforationIndex, "Petrel:Name0");
                        // arbitrarily select the first event name as perforation name
                        if (search_perforation_name.size() > 0)
                        {
                            perforation_name += "_" + search_perforation_name[0];
                            // search skin
                            auto search_perforation_skin = wellbore_completion->getConnectionExtraMetadata(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION, perforationIndex, "Petrel:Skin0");
                            if (search_perforation_skin.size() > 0)
                            {
                                try
                                {
                                    perforation_skin = search_perforation_skin[0];
                                    perforation_name = perforation_name + "__Skin_" + perforation_skin;
                                }
                                catch (const std::exception &e)
                                {
                                    vtkOutputWindowDisplayErrorText(("skin value: " + search_perforation_skin[0] + " is not numeric\n" + std::string("exception error > ") + e.what()).c_str());
                                }
                            }
                            // search diameter
                            auto search_perforation_diam = wellbore_completion->getConnectionExtraMetadata(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION, perforationIndex, "Petrel:BoreholePerforatedSection0");
                            if (search_perforation_diam.size() > 0)
                            {
                                try
                                {
                                    perforation_diameter = search_perforation_diam[0];
                                    perforation_name = perforation_name + "__Diam_" + perforation_diameter;
                                }
                                catch (const std::exception &e)
                                {
                                    vtkOutputWindowDisplayErrorText(("diameter value: " + search_perforation_diam[0] + " is not numeric\n" + std::string("exception error > ") + e.what()).c_str());
                                }
                            }
                        }
                        else
                        {
                            search_perforation_name = wellbore_completion->getConnectionExtraMetadata(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION, perforationIndex, "Sismage-CIG:Name");
                            if (search_perforation_name.size() > 0)
                            {
                                perforation_name += "_" + search_perforation_name[0];
                                auto search_perforation_skin = wellbore_completion->getConnectionExtraMetadata(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION, perforationIndex, "Sismage-CIG:Skin");
                                if (search_perforation_skin.size() > 0)
                                {
                                    try
                                    {
                                        perforation_skin = search_perforation_skin[0];
                                        perforation_name = perforation_name + "__Skin_" + perforation_skin;
                                    }
                                    catch (const std::exception &e)
                                    {
                                        vtkOutputWindowDisplayErrorText(("skin value: " + search_perforation_skin[0] + " is not numeric\n" + std::string("exception error > ") + e.what()).c_str());
                                    }
                                }
                                // search diameter
                                auto search_perforation_diam = wellbore_completion->getConnectionExtraMetadata(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION, perforationIndex, "Petrel:CompletionDiameter");
                                if (search_perforation_diam.size() > 0)
                                {
                                    try
                                    {
                                        perforation_diameter = search_perforation_diam[0];
                                        perforation_name = perforation_name + "__Diam_" + perforation_diameter;
                                    }
                                    catch (const std::exception &e)
                                    {
                                        vtkOutputWindowDisplayErrorText(("diameter value: " + search_perforation_diam[0] + " is not numeric\n" + std::string("exception error > ") + e.what()).c_str());
                                    }
                                }
                            }
                            else
                            {
                                perforation_name += "_" + wellbore_completion->getConnectionUid(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION, perforationIndex);
                            }
                        }
                        const std::string perforation_VTK_validName = this->MakeValidNodeName((perforation_name).c_str());
                        int idNode_perfo = data_assembly->AddNode(this->MakeValidNodeName(("_" + wellbore_completion->getUuid() + "_" + wellbore_completion->getConnectionUid(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION, perforationIndex)).c_str()).c_str(), idNode_completion);
                        data_assembly->SetAttribute(idNode_perfo, "label", perforation_VTK_validName.c_str());
                        int nodeType = static_cast<int>(TreeViewNodeType::Perforation);
                        data_assembly->SetAttribute(idNode_perfo, "type", std::to_string(nodeType).c_str());
                        data_assembly->SetAttribute(idNode_perfo, "connection", wellbore_completion->getConnectionUid(WITSML2_1_NS::WellboreCompletion::WellReservoirConnectionType::PERFORATION, perforationIndex).c_str());
                        data_assembly->SetAttribute(idNode_perfo, "skin", perforation_skin.c_str());
                        data_assembly->SetAttribute(idNode_perfo, "diameter", perforation_diameter.c_str());
                        data_assembly->SetAttribute(idNode_perfo, "statut", std::to_string(static_cast<int>(/*statut*/ WellboreStatut::GazProducer)).c_str());
                    }
                }
            }
        }
    }
    return result;
}

std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::searchTimeSeries(const std::string &fileName)
{
    times_step.clear();

    std::string return_message = "";
    auto *assembly = _output->GetDataAssembly();
    std::vector<EML2_NS::TimeSeries *> timeSeriesSet;
    try
    {
        timeSeriesSet = repository->getTimeSeriesSet();
    }
    catch (const std::exception &e)
    {
        return_message = return_message + "Exception in FESAPI when calling getTimeSeriesSet with file: " + fileName + " : " + e.what();
    }

    /****
     *  change property parent to times serie parent
     ****/
    for (auto const *timeSeries : timeSeriesSet)
    {
        // get properties link to Times series
        try
        {
            auto propSeries = timeSeries->getPropertySet();

            std::map<std::string, std::vector<int>> propertyName_to_nodeSet;
            for (auto const *prop : propSeries)
            {
                if (prop->getXmlTag() == RESQML2_NS::ContinuousProperty::XML_TAG ||
                    prop->getXmlTag() == RESQML2_NS::DiscreteProperty::XML_TAG)
                {
                    auto node_id = (_output->GetDataAssembly()->FindFirstNodeWithName(("_" + prop->getUuid()).c_str()));
                    if (node_id == -1)
                    {
                        return_message = return_message + "The property " + prop->getUuid() + " is not supported and consequently cannot be associated to its time series.\n";
                        continue;
                    }
                    else
                    {
                        // same node parent else not supported
                        const int nodeParent = assembly->GetParent(node_id);
                        if (nodeParent != -1)
                        {
                            propertyName_to_nodeSet[prop->getTitle()].push_back(node_id);
                            const size_t timeIndexInTimeSeries = timeSeries->getTimestampIndex(prop->getSingleTimestamp());
                            times_step.push_back(timeIndexInTimeSeries);
                            _timeSeriesUuidAndTitleToIndexAndPropertiesUuid[timeSeries->getUuid()][this->MakeValidNodeName((timeSeries->getXmlTag() + '_' + prop->getTitle()).c_str())][timeIndexInTimeSeries] = prop->getUuid();
                        }
                        else
                        {
                            return_message = return_message + "The properties of time series " + timeSeries->getUuid() + " aren't parent and is not supported.\n";
                            continue;
                        }
                    }
                }
            }
            // erase duplicate Index
            sort(times_step.begin(), times_step.end());
            times_step.erase(unique(times_step.begin(), times_step.end()), times_step.end());

            for (const auto &myPair : propertyName_to_nodeSet)
            {
                std::vector<int> property_node_set = myPair.second;

                int nodeParent = -1;
                // erase property add to treeview for group by TimeSerie
                for (auto node : property_node_set)
                {
                    nodeParent = _output->GetDataAssembly()->GetParent(node);
                    _output->GetDataAssembly()->RemoveNode(node);
                }
                std::string name = this->MakeValidNodeName((timeSeries->getXmlTag() + '_' + myPair.first).c_str());
                auto times_serie_node_id = _output->GetDataAssembly()->AddNode(("_" + timeSeries->getUuid() + name).c_str(), nodeParent);
                _output->GetDataAssembly()->SetAttribute(times_serie_node_id, "label", name.c_str());
                _output->GetDataAssembly()->SetAttribute(times_serie_node_id, "type", std::to_string(static_cast<int>(TreeViewNodeType::TimeSeries)).c_str());
            }
        }
        catch (const std::exception &e)
        {
            return_message = return_message + "Exception in FESAPI when calling getPropertySet with file: " + fileName + " : " + e.what();
        }
    }

    return return_message;
}

std::string ResqmlDataRepositoryToVtkPartitionedDataSetCollection::selectNodeId(int node)
{
    if (node != 0)
    {
        this->selectNodeIdParent(node);

        this->_currentSelection.insert(node);
        _oldSelection.erase(node);
    }
    this->selectNodeIdChildren(node);

    return "";
}

void ResqmlDataRepositoryToVtkPartitionedDataSetCollection::selectNodeIdParent(int node)
{
    auto const *assembly = _output->GetDataAssembly();

    if (assembly->GetParent(node) > 0)
    {
        this->_currentSelection.insert(assembly->GetParent(node));
        _oldSelection.erase(assembly->GetParent(node));
        this->selectNodeIdParent(assembly->GetParent(node));
    }
}
void ResqmlDataRepositoryToVtkPartitionedDataSetCollection::selectNodeIdChildren(int node)
{
    auto const *assembly = _output->GetDataAssembly();

    for (int index_child : assembly->GetChildNodes(node))
    {
        this->_currentSelection.insert(index_child);
        _oldSelection.erase(index_child);
        this->selectNodeIdChildren(index_child);
    }
}

void ResqmlDataRepositoryToVtkPartitionedDataSetCollection::clearSelection()
{
    _oldSelection = this->_currentSelection;
    this->_currentSelection.clear();
}

void ResqmlDataRepositoryToVtkPartitionedDataSetCollection::initMapper(const TreeViewNodeType type, const int node_id, const int nbProcess, const int processId)
{
    CommonAbstractObjectToVtkPartitionedDataSet *rep = nullptr;
    const std::string uuid = std::string(_output->GetDataAssembly()->GetNodeName(node_id)).substr(1);

    try
    {
        if (TreeViewNodeType::Representation == type || TreeViewNodeType::WellboreTrajectory == type || TreeViewNodeType::WellboreMarker == type || TreeViewNodeType::WellboreFrame == type || TreeViewNodeType::WellboreCompletion == type || TreeViewNodeType::Perforation == type)
        {

            COMMON_NS::AbstractObject *const result = repository->getDataObjectByUuid(uuid);
            if (TreeViewNodeType::Representation == type)
            {

                if (dynamic_cast<RESQML2_NS::AbstractIjkGridRepresentation *>(result) != nullptr)
                {
                    rep = new ResqmlIjkGridToVtkExplicitStructuredGrid(static_cast<RESQML2_NS::AbstractIjkGridRepresentation *>(result), processId, nbProcess);
                }
                else if (dynamic_cast<RESQML2_NS::Grid2dRepresentation *>(result) != nullptr)
                {
                    rep = new ResqmlGrid2dToVtkStructuredGrid(static_cast<RESQML2_NS::Grid2dRepresentation *>(result));
                }
                else if (dynamic_cast<RESQML2_NS::TriangulatedSetRepresentation *>(result) != nullptr)
                {
                    rep = new ResqmlTriangulatedSetToVtkPartitionedDataSet(static_cast<RESQML2_NS::TriangulatedSetRepresentation *>(result));
                }
                else if (dynamic_cast<RESQML2_NS::PolylineSetRepresentation *>(result) != nullptr)
                {
                    rep = new ResqmlPolylineToVtkPolyData(static_cast<RESQML2_NS::PolylineSetRepresentation *>(result));
                }
                else if (dynamic_cast<RESQML2_NS::UnstructuredGridRepresentation *>(result) != nullptr)
                {
                    rep = new ResqmlUnstructuredGridToVtkUnstructuredGrid(static_cast<RESQML2_NS::UnstructuredGridRepresentation *>(result));
                }
                else if (dynamic_cast<RESQML2_NS::SubRepresentation *>(result) != nullptr)
                {
                    RESQML2_NS::SubRepresentation *subRep = static_cast<RESQML2_NS::SubRepresentation *>(result);

                    if (dynamic_cast<RESQML2_NS::AbstractIjkGridRepresentation *>(subRep->getSupportingRepresentation(0)) != nullptr)
                    {
                        auto *supportingGrid = static_cast<RESQML2_NS::AbstractIjkGridRepresentation *>(subRep->getSupportingRepresentation(0));
                        if (_nodeIdToMapper.find(_output->GetDataAssembly()->FindFirstNodeWithName(("_" + supportingGrid->getUuid()).c_str())) == _nodeIdToMapper.end())
                        {
                            _nodeIdToMapper[_output->GetDataAssembly()->FindFirstNodeWithName(("_" + supportingGrid->getUuid()).c_str())] = new ResqmlIjkGridToVtkExplicitStructuredGrid(supportingGrid);
                        }
                        rep = new ResqmlIjkGridSubRepToVtkExplicitStructuredGrid(subRep, dynamic_cast<ResqmlIjkGridToVtkExplicitStructuredGrid *>(_nodeIdToMapper[_output->GetDataAssembly()->FindFirstNodeWithName(("_" + supportingGrid->getUuid()).c_str())]));
                    }
                    else if (dynamic_cast<RESQML2_NS::UnstructuredGridRepresentation *>(subRep->getSupportingRepresentation(0)) != nullptr)
                    {
                        auto *supportingGrid = static_cast<RESQML2_NS::UnstructuredGridRepresentation *>(subRep->getSupportingRepresentation(0));
                        if (_nodeIdToMapper.find(_output->GetDataAssembly()->FindFirstNodeWithName(("_" + supportingGrid->getUuid()).c_str())) == _nodeIdToMapper.end())
                        {
                            _nodeIdToMapper[_output->GetDataAssembly()->FindFirstNodeWithName(("_" + supportingGrid->getUuid()).c_str())] = new ResqmlUnstructuredGridToVtkUnstructuredGrid(supportingGrid);
                        }
                        rep = new ResqmlUnstructuredGridSubRepToVtkUnstructuredGrid(subRep, dynamic_cast<ResqmlUnstructuredGridToVtkUnstructuredGrid *>(_nodeIdToMapper[_output->GetDataAssembly()->FindFirstNodeWithName(("_" + supportingGrid->getUuid()).c_str())]));
                    }
                }
            }
            else if (TreeViewNodeType::WellboreTrajectory == type)
            {

                if (dynamic_cast<RESQML2_NS::WellboreTrajectoryRepresentation *>(result) != nullptr)
                {
                    rep = new ResqmlWellboreTrajectoryToVtkPolyData(static_cast<RESQML2_NS::WellboreTrajectoryRepresentation *>(result));
                }
            }
            else if (TreeViewNodeType::WellboreCompletion == type)
            {
                if (dynamic_cast<witsml2_1::WellboreCompletion*>(result) != nullptr)
                {
                    _nodeIdToMapperSet[node_id] = new WitsmlWellboreCompletionToVtkPartitionedDataSet(static_cast<witsml2_1::WellboreCompletion*>(result));
                }
            }
            else if (TreeViewNodeType::Perforation == type)
            {
                const int node_parent = _output->GetDataAssembly()->GetParent(node_id);
                if (static_cast<WitsmlWellboreCompletionToVtkPartitionedDataSet*>(_nodeIdToMapperSet[node_parent]))
                {
                    const char * p_connection;
                    _output->GetDataAssembly()->GetAttribute(node_id, "connection", p_connection);
                    const char *name;
                    _output->GetDataAssembly()->GetAttribute(node_id, "label", name);
                    const char *skin_s;
                    _output->GetDataAssembly()->GetAttribute(node_id, "skin", skin_s);
                    const double skin = std::strtod(skin_s, nullptr);
                    /*
                    const char* statut_s;
                    output->GetDataAssembly()->GetAttribute(node_id, "statut", statut_s);
                    const WellboreStatut statut = static_cast<WellboreStatut>(std::stoi(skin_s));
                    */
                    const WellboreStatut statut = WellboreStatut::GazInjecter;
                    if(!_nodeIdToMapperSet[node_parent]->existUuid(p_connection))
                    {
                        (static_cast<WitsmlWellboreCompletionToVtkPartitionedDataSet*>(_nodeIdToMapperSet[node_parent]))->addPerforation(p_connection, name, skin, statut);
                    }
                    return;
  /*
  for (auto item : (static_cast<WitsmlWellboreCompletionToVtkPartitionedDataSet*>(_nodeIdToMapperSet[node_parent]))->getMapperSet())
                    {
                        WitsmlWellboreCompletionPerforationToVtkPolyData* perforation = dynamic_cast<WitsmlWellboreCompletionPerforationToVtkPolyData*>(item);
                        if (perforation->getTitle() == std::string(name) && perforation->getConnectionuid() == std::string(value))
                        {
                            rep = perforation;
                        }
                    }
                    */
                }
            }
            else if (TreeViewNodeType::WellboreMarker == type)
            {
                if (dynamic_cast<RESQML2_NS::WellboreMarkerFrameRepresentation *>(result) != nullptr)
                {
                    rep = new ResqmlWellboreMarkerFrameToVtkPartitionedDataSet(static_cast<RESQML2_NS::WellboreMarkerFrameRepresentation *>(result));
                }
            }
            else if (TreeViewNodeType::WellboreFrame == type)
            {
                if (dynamic_cast<RESQML2_NS::WellboreFrameRepresentation *>(result) != nullptr)
                {
                    rep = new ResqmlWellboreFrameToVtkPartitionedDataSet(static_cast<RESQML2_NS::WellboreFrameRepresentation *>(result));
                }
                else
                {
                    return;
                }
            }
            else
            {
                return;
            }
            _nodeIdToMapper[node_id] = rep;
            return;
        }
    }
    catch (const std::exception &e)
    {
        vtkOutputWindowDisplayErrorText(("Error when initialize uuid: " + uuid + "\n" + e.what()).c_str());
    }
}

void ResqmlDataRepositoryToVtkPartitionedDataSetCollection::loadMapper(const TreeViewNodeType type, const int node_id /*const std::string& uuid*/, double time)
{
    auto const *assembly = _output->GetDataAssembly();
    const std::string uuid = std::string(_output->GetDataAssembly()->GetNodeName(node_id)).substr(1);
    if (type == TreeViewNodeType::TimeSeries)
    {
        try
        {
            std::string ts_uuid = uuid.substr(0, 36);
            std::string node_name = uuid.substr(36);

            auto const *assembly = _output->GetDataAssembly();
            const int node_parent = assembly->GetParent(_output->GetDataAssembly()->FindFirstNodeWithName(("_" + uuid).c_str()));
            if (_nodeIdToMapper[node_parent])
            {
                static_cast<ResqmlAbstractRepresentationToVtkPartitionedDataSet *>(_nodeIdToMapper[node_parent])->addDataArray(_timeSeriesUuidAndTitleToIndexAndPropertiesUuid[ts_uuid][node_name][time]);
            }
            return;
        }
        catch (const std::exception &e)
        {
            vtkOutputWindowDisplayErrorText(("Error when load Time Series property marker uuid: " + uuid + "\n" + e.what()).c_str());
            return;
        }
    }
    else
    {

        COMMON_NS::AbstractObject *const result = repository->getDataObjectByUuid(uuid);

        try
        { // load Wellbore Marker
            if (dynamic_cast<RESQML2_NS::WellboreMarker *>(result) != nullptr)
            {
                auto const *assembly = _output->GetDataAssembly();
                const int node_parent = assembly->GetParent(_output->GetDataAssembly()->FindFirstNodeWithName(("_" + uuid).c_str()));
                static_cast<ResqmlWellboreMarkerFrameToVtkPartitionedDataSet *>(_nodeIdToMapper[node_parent])->addMarker(uuid, markerOrientation, markerSize);
                return;
            }
        }
        catch (const std::exception &e)
        {
            vtkOutputWindowDisplayErrorText(("Error when load wellbore marker uuid: " + uuid + "\n" + e.what()).c_str());
            return;
        }
        try
        { // load property
            if (dynamic_cast<RESQML2_NS::AbstractValuesProperty *>(result) != nullptr)
            {
                auto const *assembly = _output->GetDataAssembly();
                const int node_parent = assembly->GetParent(node_id); // node_id
                if (dynamic_cast<ResqmlWellboreFrameToVtkPartitionedDataSet *>(_nodeIdToMapper[node_parent]) != nullptr)
                {
                    static_cast<ResqmlWellboreFrameToVtkPartitionedDataSet *>(_nodeIdToMapper[node_parent])->addChannel(uuid, static_cast<resqml2::AbstractValuesProperty *>(result));
                }
                else if (dynamic_cast<ResqmlTriangulatedSetToVtkPartitionedDataSet *>(_nodeIdToMapper[node_parent]) != nullptr)
                {
                    static_cast<ResqmlTriangulatedSetToVtkPartitionedDataSet *>(_nodeIdToMapper[node_parent])->addDataArray(uuid);
                }
                else
                {
                    static_cast<ResqmlAbstractRepresentationToVtkPartitionedDataSet *>(_nodeIdToMapper[node_parent])->addDataArray(uuid);
                }
                return;
            }
        }
        catch (const std::exception &e)
        {
            vtkOutputWindowDisplayErrorText(("Error when load property uuid: " + uuid + "\n" + e.what()).c_str());
            return;
        }

        try
        { // load representation

            _nodeIdToMapper[node_id]->loadVtkObject(); // node_id

            return;
        }
        catch (const std::exception &e)
        {
            vtkOutputWindowDisplayErrorText(("Error when rendering uuid: " + uuid + "\n" + e.what()).c_str());
            return;
        }
    }
}


/**
* delete oldSelection mapper
*/
void ResqmlDataRepositoryToVtkPartitionedDataSetCollection::deleteMapper(double p_time)
{
    // initialization the output (VtkPartitionedDatasSetCollection) with same vtkDataAssembly
    vtkSmartPointer<vtkDataAssembly> w_Assembly = vtkSmartPointer<vtkDataAssembly>::New();
    w_Assembly->DeepCopy(_output->GetDataAssembly());
    _output = vtkSmartPointer<vtkPartitionedDataSetCollection>::New();
    _output->SetDataAssembly(w_Assembly);

    // delete unchecked object
    for (const int w_nodeId : _oldSelection)
    {
        // retrieval of object type for nodeid
        int w_valueType;
        w_Assembly->GetAttribute(w_nodeId, "type", w_valueType);
        TreeViewNodeType valueType = static_cast<TreeViewNodeType>(w_valueType);

        // retrieval of object UUID for nodeId
        const std::string uuid_unselect = std::string(w_Assembly->GetNodeName(w_nodeId)).substr(1);

        if (valueType == TreeViewNodeType::TimeSeries)
        { // TimeSerie properties deselection
            std::string w_timeSeriesuuid = uuid_unselect.substr(0, 36);
            std::string w_nodeName = uuid_unselect.substr(36);

            const int w_nodeParent = w_Assembly->GetParent(w_Assembly->FindFirstNodeWithName(("_" + uuid_unselect).c_str()));
            if (_nodeIdToMapper.find(w_nodeParent) != _nodeIdToMapper.end())
            {
                static_cast<ResqmlWellboreMarkerFrameToVtkPartitionedDataSet*>(_nodeIdToMapper[w_nodeParent])->deleteDataArray(_timeSeriesUuidAndTitleToIndexAndPropertiesUuid[w_timeSeriesuuid][w_nodeName][p_time]);
            }
        }
        else if (valueType == TreeViewNodeType::Properties)
        {
            const int w_nodeParent = w_Assembly->GetParent(w_nodeId);
            try
            {
                if (_nodeIdToMapper.find(w_nodeParent) != _nodeIdToMapper.end())
                {
                    ResqmlAbstractRepresentationToVtkPartitionedDataSet* abstractRepresentationMapper = static_cast<ResqmlAbstractRepresentationToVtkPartitionedDataSet*>(_nodeIdToMapper[w_nodeParent]);
                    abstractRepresentationMapper->deleteDataArray(std::string(w_Assembly->GetNodeName(w_nodeId)).substr(1));
                }
            }
            catch (const std::exception& e)
            {
                vtkOutputWindowDisplayErrorText(("Error in property unload for uuid: " + uuid_unselect + "\n" + e.what()).c_str());
            }
        }
        else if (valueType == TreeViewNodeType::WellboreMarker
            )
        {
            const int w_nodeParent = w_Assembly->GetParent(w_nodeId);
            try
            {
                if (_nodeIdToMapper.find(w_nodeParent) != _nodeIdToMapper.end())
                {
                    ResqmlWellboreMarkerFrameToVtkPartitionedDataSet* markerFrame = static_cast<ResqmlWellboreMarkerFrameToVtkPartitionedDataSet*>(_nodeIdToMapper[w_nodeParent]);
                    markerFrame->removeMarker(std::string(w_Assembly->GetNodeName(w_nodeId)).substr(1));
                }
            }
            catch (const std::exception& e)
            {
                vtkOutputWindowDisplayErrorText(("Error in property unload for uuid: " + uuid_unselect + "\n" + e.what()).c_str());
            }
        }
        else if (valueType == TreeViewNodeType::SubRepresentation)
        {
            try
            {
                if (_nodeIdToMapper.find(w_nodeId) != _nodeIdToMapper.end())
                {
                    std::string uuid_supporting_grid = "";
                    if (dynamic_cast<ResqmlUnstructuredGridSubRepToVtkUnstructuredGrid*>(_nodeIdToMapper[w_nodeId]) != nullptr)
                    {
                        uuid_supporting_grid = static_cast<ResqmlUnstructuredGridSubRepToVtkUnstructuredGrid*>(_nodeIdToMapper[w_nodeId])->unregisterToMapperSupportingGrid();
                    }
                    else if (dynamic_cast<ResqmlIjkGridSubRepToVtkExplicitStructuredGrid*>(_nodeIdToMapper[w_nodeId]) != nullptr)
                    {
                        uuid_supporting_grid = static_cast<ResqmlIjkGridSubRepToVtkExplicitStructuredGrid*>(_nodeIdToMapper[w_nodeId])->unregisterToMapperSupportingGrid();
                    }
                    delete _nodeIdToMapper[w_nodeId];
                    _nodeIdToMapper.erase(w_nodeId);
                }
                else
                {
                    vtkOutputWindowDisplayErrorText(("Error in deselection for uuid: " + uuid_unselect + "\n").c_str());
                }
            }
            catch (const std::exception& e)
            {
                vtkOutputWindowDisplayErrorText(("Error in subrepresentation unload for uuid: " + uuid_unselect + "\n" + e.what()).c_str());
            }
        }
        else if (valueType == TreeViewNodeType::Representation ||
            valueType == TreeViewNodeType::WellboreTrajectory ||
            valueType == TreeViewNodeType::WellboreFrame ||
            valueType == TreeViewNodeType::WellboreChannel ||
            valueType == TreeViewNodeType::WellboreMarkerFrame
            )
        {
            try
            {
                if (_nodeIdToMapper.find(w_nodeId) != _nodeIdToMapper.end())
                {
                    delete _nodeIdToMapper[w_nodeId];
                    _nodeIdToMapper.erase(w_nodeId);
                }
            }
            catch (const std::exception& e)
            {
                vtkOutputWindowDisplayErrorText(("Error for unload uuid: " + uuid_unselect + "\n" + e.what()).c_str());
            }
        }
         else if (valueType == TreeViewNodeType::Perforation
            )
            // delete child of CommonAbstractObjectSetToVtkPartitionedDataSetSet
            {
                const int w_nodeParent = w_Assembly->GetParent(w_nodeId);
                try
                {
                    if (_nodeIdToMapperSet.find(w_nodeParent) != _nodeIdToMapperSet.end())
                    {
                        const char* w_connection;
                        _output->GetDataAssembly()->GetAttribute(w_nodeId, "connection", w_connection);
                        _nodeIdToMapperSet[w_nodeParent]->removeCommonAbstractObjectToVtkPartitionedDataSet(w_connection);
                    }
                }
                catch (const std::exception& e)
                {
                    vtkOutputWindowDisplayErrorText(("Error in WellboreCompletion unload for uuid: " + uuid_unselect + "\n" + e.what()).c_str());
                }
                }
         else if (valueType == TreeViewNodeType::WellboreCompletion
             )
             {
                 try
                 {
                     if (_nodeIdToMapperSet.find(w_nodeId) != _nodeIdToMapperSet.end())
                     {
                         delete _nodeIdToMapperSet[w_nodeId];
                         _nodeIdToMapperSet.erase(w_nodeId);
                     }
                 }
                 catch (const std::exception& e)
                 {
                     vtkOutputWindowDisplayErrorText(("Error in WellboreCompletion unload for uuid: " + uuid_unselect + "\n" + e.what()).c_str());
                 }
                 }
    }

}

vtkPartitionedDataSetCollection *ResqmlDataRepositoryToVtkPartitionedDataSetCollection::getVtkPartitionedDatasSetCollection(const double time, const int nbProcess, const int processId)
{
    deleteMapper(time);
    // vtkParitionedDataSetCollection - hierarchy - build
    // foreach selection node init objetc
    for (const int node_selection : this->_currentSelection)
    {
        int value_type;
        _output->GetDataAssembly()->GetAttribute(node_selection, "type", value_type);
        TreeViewNodeType w_type = static_cast<TreeViewNodeType>(value_type);
        const std::string uuid = std::string(_output->GetDataAssembly()->GetNodeName(node_selection)).substr(1);

        if (w_type == TreeViewNodeType::WellboreCompletion)
        {
            // initialize mapper
            if (_nodeIdToMapperSet.find(node_selection) == _nodeIdToMapperSet.end())
            {
                initMapper(w_type, node_selection /*uuid*/, nbProcess, processId);
            }

         }
        else {
            // initialize mapper
            if (_nodeIdToMapper.find(node_selection) == _nodeIdToMapper.end())
            {
                initMapper(w_type, node_selection /*uuid*/, nbProcess, processId);
            }
        }
    }

    unsigned int index = 0; // index for PartionedDatasSet
    // foreach selection node load object
    for (const int node_selection : this->_currentSelection)
    {
        int value_type;
        _output->GetDataAssembly()->GetAttribute(node_selection, "type", value_type);
        TreeViewNodeType w_type = static_cast<TreeViewNodeType>(value_type);
        const std::string uuid = std::string(_output->GetDataAssembly()->GetNodeName(node_selection)).substr(1);

        if (w_type == TreeViewNodeType::WellboreCompletion)
        {
            // load mapper representation
            if (_nodeIdToMapperSet.find(node_selection) != _nodeIdToMapperSet.end())
            {
                _nodeIdToMapperSet[node_selection]->loadVtkObject();
                for (auto partition : _nodeIdToMapperSet[node_selection]->getMapperSet())
                {
                    _output->SetPartitionedDataSet(index, partition->getOutput());
                    GetAssembly()->AddDataSetIndex(node_selection, index);
                    _output->GetMetaData(index++)->Set(vtkCompositeDataSet::NAME(), partition->getTitle() + '(' + partition->getUuid() + ')');
                }
            }
        }
        else if (w_type == TreeViewNodeType::Representation || 
            w_type == TreeViewNodeType::SubRepresentation || 
            w_type == TreeViewNodeType::Properties ||
            w_type == TreeViewNodeType::WellboreTrajectory ||
            w_type == TreeViewNodeType::WellboreFrame ||
            w_type == TreeViewNodeType::WellboreChannel ||
            w_type == TreeViewNodeType::WellboreMarkerFrame ||
            w_type == TreeViewNodeType::WellboreMarker ||
            w_type == TreeViewNodeType::TimeSeries 
            )
        {
            // load mapper representation
            if (_nodeIdToMapper.find(node_selection) != _nodeIdToMapper.end())
            {
                if (_nodeIdToMapper[node_selection]->getOutput()->GetNumberOfPartitions() < 1)
                {
                    loadMapper(w_type, node_selection /*uuid*/, time);
                }
                _output->SetPartitionedDataSet(index, _nodeIdToMapper[node_selection]->getOutput());
                GetAssembly()->AddDataSetIndex(node_selection, index);
                _output->GetMetaData(index++)->Set(vtkCompositeDataSet::NAME(), _nodeIdToMapper[node_selection]->getTitle() + '(' + _nodeIdToMapper[node_selection]->getUuid() + ')');
            }
            else
            {
                loadMapper(static_cast<TreeViewNodeType>(value_type), node_selection /*uuid*/, time);
            }
        }
    }

    _output->Modified();
    return _output;
}

void ResqmlDataRepositoryToVtkPartitionedDataSetCollection::setMarkerOrientation(bool orientation)
{
    markerOrientation = orientation;
}

void ResqmlDataRepositoryToVtkPartitionedDataSetCollection::setMarkerSize(int size)
{
    markerSize = size;
}
