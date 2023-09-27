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
#include "WitsmlWellboreCompletionToVtkPartitionedDataSet.h"

#include <vtkInformation.h>

#include <fesapi/witsml2_1/WellboreCompletion.h>
#include <fesapi/resqml2/MdDatum.h>
#include <fesapi/resqml2/WellboreFeature.h>
#include <fesapi/resqml2/WellboreInterpretation.h>
#include <fesapi/resqml2/WellboreTrajectoryRepresentation.h>

#include "WitsmlWellboreCompletionPerforationToVtkPolyData.h"

//----------------------------------------------------------------------------
WitsmlWellboreCompletionToVtkPartitionedDataSet::WitsmlWellboreCompletionToVtkPartitionedDataSet(WITSML2_1_NS::WellboreCompletion *completion, int proc_number, int max_proc)
	: CommonAbstractObjectToVtkPartitionedDataSet(completion,
											   proc_number,
											   max_proc)
{
	for (auto* interpretation : completion->getWellbore()->getResqmlWellboreFeature(0)->getInterpretationSet()) {
		if (dynamic_cast<RESQML2_NS::WellboreInterpretation*>(interpretation) != nullptr) {
			auto *wellbore_interpretation = static_cast<resqml2::WellboreInterpretation*>(interpretation);
			this->wellboreTrajectory = wellbore_interpretation->getWellboreTrajectoryRepresentation(0);
			continue;
		}
	}

	this->vtkData = vtkSmartPointer<vtkPartitionedDataSet>::New();
	this->vtkData->Modified();
}

//----------------------------------------------------------------------------
WITSML2_1_NS::WellboreCompletion const* WitsmlWellboreCompletionToVtkPartitionedDataSet::getResqmlData() const
{
	return static_cast<WITSML2_1_NS::WellboreCompletion const*>(resqmlData);
}

//----------------------------------------------------------------------------
resqml2::WellboreTrajectoryRepresentation const* WitsmlWellboreCompletionToVtkPartitionedDataSet::getWellboreTrajectory() const
{
	return this->wellboreTrajectory;
}

//----------------------------------------------------------------------------
void WitsmlWellboreCompletionToVtkPartitionedDataSet::loadVtkObject()
{
	for (int idx = 0; idx < this->list_perforation.size(); ++idx)
	{
		this->vtkData->SetPartition(idx, this->list_perforation[idx]->getOutput()->GetPartitionAsDataObject(0));
		this->vtkData->GetMetaData(idx)->Set(vtkCompositeDataSet::NAME(), this->list_perforation[idx]->getTitle().c_str());
	}
}

//----------------------------------------------------------------------------
void WitsmlWellboreCompletionToVtkPartitionedDataSet::addPerforation(const std::string& uuid)
{
	if (std::find_if(list_perforation.begin(), list_perforation.end(), [uuid](WitsmlWellboreCompletionPerforationToVtkPolyData const* perforation) { return perforation->getUuid() == uuid; }) == list_perforation.end())
	{
		this->list_perforation.push_back(new WitsmlWellboreCompletionPerforationToVtkPolyData(this->getResqmlData(), this->getWellboreTrajectory(), uuid));
		this->loadVtkObject();
	}
}

//----------------------------------------------------------------------------
void WitsmlWellboreCompletionToVtkPartitionedDataSet::removePerforation(const std::string& uuid)
{
	list_perforation.erase(
		std::remove_if(list_perforation.begin(), list_perforation.end(), [uuid](WitsmlWellboreCompletionPerforationToVtkPolyData const* perforation) { return perforation->getUuid() == uuid; }),
		list_perforation.end());
	this->loadVtkObject();
}
