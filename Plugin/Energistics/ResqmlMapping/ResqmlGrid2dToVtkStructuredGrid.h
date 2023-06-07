﻿/*-----------------------------------------------------------------------
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
#ifndef __ResqmlGrid2dToVtkStructuredGrid__h__
#define __ResqmlGrid2dToVtkStructuredGrid__h__

/** @brief	transform a resqml Grid2D representation to VtkStructuredGrid
 */

// include system
#include <unordered_map>
#include <string>

// include VTK
#include <vtkSmartPointer.h> 
#include <vtkStructuredGrid.h>

#include "ResqmlAbstractRepresentationToVtkPartitionedDataSet.h"

namespace RESQML2_NS
{
	class Grid2dRepresentation;
}

class ResqmlGrid2dToVtkStructuredGrid : public ResqmlAbstractRepresentationToVtkPartitionedDataSet 
{
public:
	/**
	* Constructor
	*/
	explicit ResqmlGrid2dToVtkStructuredGrid(RESQML2_NS::Grid2dRepresentation * grid2D, int proc_number = 0, int max_proc = 1);

	/**
	* load vtkDataSet with resqml data
	*/
	void loadVtkObject() override;

protected:
	RESQML2_NS::Grid2dRepresentation const* getResqmlData() const;
};
#endif