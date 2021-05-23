/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2011-2015  Regents of the University of California.
 *
 * This file is part of ndnSIM. See AUTHORS for complete list of ndnSIM authors and
 * contributors.
 *
 * ndnSIM is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ndnSIM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndnSIM, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 **/

#ifndef NDN_PRODUCER_H
#define NDN_PRODUCER_H

#include "ns3/ndnSIM/model/ndn-common.hpp"

#include "ndn-app.hpp"
#include "ns3/ndnSIM/model/ndn-common.hpp"

#include "ns3/nstime.h"
#include "ns3/ptr.h"
using namespace std;
namespace ns3 {
namespace ndn {

/**
 * @ingroup ndn-apps
 * @brief A simple Interest-sink applia simple Interest-sink application
 *
 * A simple Interest-sink applia simple Interest-sink application,
 * which replying every incoming Interest with Data packet with a specified
 * size and name same as in Interest.cation, which replying every incoming Interest
 * with Data packet with a specified size and name same as in Interest.
 */
class ProducerCompute : public App {
public:
  static TypeId
  GetTypeId(void);

  ProducerCompute();

  // inherited from NdnApp
  virtual void
  OnInterest(shared_ptr<const Interest> interest);
  
  // Atif-Code: Start

  enum ServiceStatus {
        Running = 1,
        Paused = 2,
        Completed = 3
  };

  struct ServiceInfo
  {
    int service_id; // each consumer should forward unique service id because the service name could be same, but remember service id only used to fetch the running service, every other logic will be handled by the service name as per NDN rituals
    string service_name;
    int start_time; // in milisecond
    int node_id;
    int run_time;
    ProducerCompute::ServiceStatus service_status;
    double resource_utilization_value;
  };
  vector<ProducerCompute::ServiceInfo> service_info_collection; 

  tuple<int,int,int,int> 
  StartServiceExecution(shared_ptr<const Interest> interest);

  virtual void 
  EndServiceExecution(int service_id,int resource_value);

  virtual void 
  PauseServiceExecution(int service_id);

  int
  AddToServiceSatusCollection(string service_name, int start_time, int node_id,int run_time,ServiceStatus service_status);

  virtual ProducerCompute::ServiceInfo
  UpdateServiceStatus(int service_id, ProducerCompute::ServiceStatus updated_service_status);
  
  virtual ProducerCompute::ServiceInfo
  FetchFromServiceCollectionByNameId(string service_name, int service_id);

  virtual ProducerCompute::ServiceInfo
  FetchFromServiceCollectionById(int service_id);

  virtual vector<ProducerCompute::ServiceInfo>
  FetchFromServiceCollectionByName(string service_name);

  virtual vector<ProducerCompute::ServiceInfo>
  FetchFromServiceCollectionByStatus(ProducerCompute::ServiceStatus service_status);

  virtual int
  GetServiceIndexInCollection(int service_id);

  virtual tuple<string,string,string> 
  GetServiceInformation(shared_ptr<const Interest> interest);

  virtual int 
  OccupyNodeResources(int service_type);

  virtual void 
  ReleaseNodeResources(int resource_value);

  virtual int 
  GetServiceExecutionTime(int service_type);

  virtual double 
  GetComputeResourceUtilizationValue(int service_type);

 

  // Atif-Code: End

protected:
  // inherited from Application base class.
  virtual void
  StartApplication(); // Called at time specified by Start

  virtual void
  StopApplication(); // Called at time specified by Stop

  // Atif-Code: Start
  virtual void  
  SendData(shared_ptr<const Data> data,int service_id, int run_time, int resource_value);


  int 
  CurrentTime();

  ns3::Ptr<ns3::Node> 
  GetCurrentNode();

  // Atif-Code: End

private:
  Name m_prefix;
  Name m_postfix;
  uint32_t m_virtualPayloadSize;
  Time m_freshness;

  uint32_t m_signature;
  Name m_keyLocator;

  // Atif Code: Start
  int m_service_id=0;
  // Atif Code: End
};

} // namespace ndn
} // namespace ns3

#endif // NDN_PRODUCER_H
