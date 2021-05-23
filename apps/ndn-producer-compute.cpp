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

#include "ndn-producer-compute.hpp"
#include "ns3/log.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include <boost/algorithm/string.hpp>
#include "model/ndn-l3-protocol.hpp"
#include "helper/ndn-fib-helper.hpp"

#include <memory>

#include <ns3/node-list.h>
#include <ns3/node.h>

#include "ns3/core-module.h"

using namespace std;
NS_LOG_COMPONENT_DEFINE("ndn.ProducerCompute");

namespace ns3 {
namespace ndn {

NS_OBJECT_ENSURE_REGISTERED(ProducerCompute);

TypeId
ProducerCompute::GetTypeId(void)
{
  static TypeId tid =
    TypeId("ns3::ndn::ProducerCompute")
      .SetGroupName("Ndn")
      .SetParent<App>()
      .AddConstructor<ProducerCompute>()
      .AddAttribute("Prefix", "Prefix, for which producer has the data", StringValue("/"),
                    MakeNameAccessor(&ProducerCompute::m_prefix), MakeNameChecker())
      .AddAttribute(
         "Postfix",
         "Postfix that is added to the output data (e.g., for adding producer-uniqueness)",
         StringValue("/"), MakeNameAccessor(&ProducerCompute::m_postfix), MakeNameChecker())
      .AddAttribute("PayloadSize", "Virtual payload size for Content packets", UintegerValue(1024),
                    MakeUintegerAccessor(&ProducerCompute::m_virtualPayloadSize),
                    MakeUintegerChecker<uint32_t>())
      .AddAttribute("Freshness", "Freshness of data packets, if 0, then unlimited freshness",
                    TimeValue(Seconds(0)), MakeTimeAccessor(&ProducerCompute::m_freshness),
                    MakeTimeChecker())
      .AddAttribute(
         "Signature",
         "Fake signature, 0 valid signature (default), other values application-specific",
         UintegerValue(0), MakeUintegerAccessor(&ProducerCompute::m_signature),
         MakeUintegerChecker<uint32_t>())
      .AddAttribute("KeyLocator",
                    "Name to be used for key locator.  If root, then key locator is not used",
                    NameValue(), MakeNameAccessor(&ProducerCompute::m_keyLocator), MakeNameChecker());
  return tid;
}

ProducerCompute::ProducerCompute()
{
  NS_LOG_FUNCTION_NOARGS();
}

// inherited from Application base class.
void
ProducerCompute::StartApplication()
{
  NS_LOG_FUNCTION_NOARGS();
  App::StartApplication();

  cout<<"Node_Id"<<"\t"<<"Service_Id"<<"\t"<<"Service_Type"<<"\t"<<"Resource_Value"<<"\t"<<"Service_Run_Time"<<"\t"<<"Node_Inital_Compute_Resources"<<"\t"<<"Node_Compute_Resources"<<"\t"<<"Node_Compute_Capacity"<<endl;
  FibHelper::AddRoute(GetNode(), m_prefix, m_face, 0);
}

void
ProducerCompute::StopApplication()
{
  NS_LOG_FUNCTION_NOARGS();

  App::StopApplication();
}

void
ProducerCompute::OnInterest(shared_ptr<const Interest> interest)
{

  App::OnInterest(interest); // tracing inside

  NS_LOG_FUNCTION(this << interest);

  if (!m_active)
    return;

  Name dataName(interest->getName());
  // dataName.append(m_postfix);
  // dataName.appendVersion();

  auto data = make_shared<Data>();
  data->setName(dataName);
  data->setFreshnessPeriod(::ndn::time::milliseconds(m_freshness.GetMilliSeconds()));

  data->setContent(make_shared< ::ndn::Buffer>(m_virtualPayloadSize));

  Signature signature;
  SignatureInfo signatureInfo(static_cast< ::ndn::tlv::SignatureTypeValue>(255));

  if (m_keyLocator.size() > 0) {
    signatureInfo.setKeyLocator(m_keyLocator);
  }

  signature.setInfo(signatureInfo);
  signature.setValue(::ndn::makeNonNegativeIntegerBlock(::ndn::tlv::SignatureValue, m_signature));

  data->setSignature(signature);
  
  tuple<int,int,int,int> service_info=StartServiceExecution(interest);

  int  service_id=get<0>(service_info);
  int  service_type=get<1>(service_info);
  int  run_time=get<2>(service_info);
  int  resource_value=get<3>(service_info);
  ns3::Time time = ns3::MilliSeconds (run_time);

  ns3::Ptr<ns3::Node> currentNode=GetCurrentNode();
  cout<<currentNode->GetId()<<"\t"<<service_id<<"\t\t"<<service_type<<"\t\t"<<resource_value<<"\t\t"<<run_time<<"\t\t\t"<<currentNode->m_inital_compute_resources<<"\t\t\t\t"<<currentNode->m_compute_resources<<"\t\t\t"<<currentNode->m_compute_capacity<<endl;
  
  // set event setting for pause service execution
  ns3::Simulator::Schedule (time, &ProducerCompute::SendData, this, data,service_id,run_time,resource_value);
}

void  //Send Data code seperated from OnInerest to delay the forwarding based on computation time. 
ProducerCompute::SendData(shared_ptr<const Data> data,int service_id, int run_time, int resource_value)
{

  EndServiceExecution(service_id,resource_value);


  NS_LOG_INFO("node(" << GetNode()->GetId() << ") responding with Data: " << data->getName());

  // to create real wire encoding
  data->wireEncode();

  m_transmittedDatas(data, this, m_face);
  m_appLink->onReceiveData(*data);
}

// Atif-Code: 

tuple<int,int,int,int> 
ProducerCompute::StartServiceExecution(shared_ptr<const Interest> interest)
{
  // Start Execution based on service name, type, and priority


  tuple<string,string,string> service_info_tupple = GetServiceInformation(interest);
  string service_name=get<0>(service_info_tupple);
    
  ns3::Ptr<ns3::UniformRandomVariable> m_rand(ns3::CreateObject<ns3::UniformRandomVariable>());
  int service_type=m_rand->GetValue(1,5); //get<1>(service_info_tupple); TODO:
  
  string service_priority=get<2>(service_info_tupple);

  ns3::Ptr<ns3::Node> current_node=GetCurrentNode();
  int start_time=CurrentTime();

  
  int run_time=CurrentTime();
  int service_id=AddToServiceSatusCollection(service_name, start_time, current_node->GetId(),run_time, (ServiceStatus)1);
  int resource_value=OccupyNodeResources(service_type);
  
  tuple<int,int,int,int> service_info={service_id,service_type,run_time,resource_value};
  
  return service_info;
}

void 
ProducerCompute::PauseServiceExecution(int service_id)
{
  // TODO: Pause service user case: High priority service arrived then pause low priority service. ResumeServiceExecution is also needed
}

void 
ProducerCompute::EndServiceExecution(int service_id,int resource_value)
{
    UpdateServiceStatus(service_id,(ServiceStatus)3);
    ReleaseNodeResources(resource_value);
}

int 
ProducerCompute::OccupyNodeResources(int service_type)
{
  ns3::Ptr<ns3::Node> current_node=GetCurrentNode();
  double resource_utilization_value=GetComputeResourceUtilizationValue(service_type);

  current_node->m_compute_resources=current_node->m_compute_resources-resource_utilization_value;
  current_node->m_compute_capacity=((double)current_node->m_compute_resources / (double)current_node->m_inital_compute_resources)*100.0;

 // cout<<"OccupyNodeResources(): Current Node Id: "<<current_node->GetId()<<" initial computational resources: "<<current_node->m_inital_compute_resources<<" remaining computation resources: "<<current_node->m_compute_resources<<" remaining computational capacity: "<<current_node->m_compute_capacity<<endl;

  return resource_utilization_value;
}

void 
ProducerCompute::ReleaseNodeResources(int resource_value)
{
  ns3::Ptr<ns3::Node> current_node=GetCurrentNode();
  current_node->m_compute_resources=current_node->m_compute_resources+resource_value;
    current_node->m_compute_capacity=((double)current_node->m_compute_resources / (double)current_node->m_inital_compute_resources)*100.0;

 // cout<<"ReleaseNodeResources(): Current Node Id: "<<current_node->GetId()<<" initial computational resources: "<<current_node->m_inital_compute_resources<<" remaining computation resources: "<<current_node->m_compute_resources<<" remaining computational capacity: "<<current_node->m_compute_capacity<<endl;
}

tuple<string,string,string> 
ProducerCompute::GetServiceInformation(shared_ptr<const Interest> interest)
{

  string interest_name=interest->getName().toUri();
  vector<string> name_collection;
  boost::split(name_collection, interest_name, boost::is_any_of("7C"));
  string service_info=name_collection[2]; // service info will always be at index 1 (make sure that consumer application set service info after vertical pipe (|))
  vector<string> service_info_collection;
  boost::split(service_info_collection, service_info, boost::is_any_of("/")); // make sure that consumer application seperate the service info by "/"
  //cout<<"GetServiceInformation(): Service Name: "+service_info_collection[0]+" Service Tyype:  "+service_info_collection[1]+" Service Priority:  "+service_info_collection[2]<<endl;
  tuple<string,string,string> service_info_tupple= {service_info_collection[0],service_info_collection[1],service_info_collection[2]};
  
  return service_info_tupple;
}

double // the value will be in percentage
ProducerCompute::GetComputeResourceUtilizationValue(int service_type)
{
  ns3::Ptr<ns3::UniformRandomVariable> m_rand(ns3::CreateObject<ns3::UniformRandomVariable>());

  double resource_utilization_value=m_rand->GetValue((service_type*5), (service_type*10));

  return resource_utilization_value;
}

int // the value will be in millisecond
ProducerCompute::GetServiceExecutionTime(int service_type)
{
  ns3::Ptr<ns3::UniformRandomVariable> m_rand(ns3::CreateObject<ns3::UniformRandomVariable>());

  int resource_utilization_value=m_rand->GetValue((service_type*5), (service_type*10));

  return resource_utilization_value;
}



int
ProducerCompute::AddToServiceSatusCollection(string service_name, int start_time, int node_id,int run_time, ServiceStatus service_status)
{
  int service_id=m_service_id++;
  ProducerCompute::ServiceInfo service_info= {service_id, service_name,start_time,node_id,run_time,service_status};
  
  service_info_collection.push_back(service_info);
 
  return service_id;
}

ProducerCompute::ServiceInfo
ProducerCompute::FetchFromServiceCollectionByNameId(string service_name, int service_id)
{

  // TODO: improve further
    vector<ServiceInfo>::iterator service_info = find_if(service_info_collection.begin(), service_info_collection.end(), [&](const ServiceInfo & x) {
     return x.service_id == service_id && x.service_name==service_name;
   });
  return *service_info;
}
ProducerCompute::ServiceInfo
ProducerCompute::FetchFromServiceCollectionById(int service_id)
{
  // TODO: improve further
  vector<ServiceInfo>::iterator service_info =find_if(service_info_collection.begin(), service_info_collection.end(), [&](const ServiceInfo & x) {
    return x.service_id == service_id;
  });
  return *service_info;
}

vector<ProducerCompute::ServiceInfo>
ProducerCompute::FetchFromServiceCollectionByName(string service_name)
{
  // TODO: improve this searching mechanism
  vector<ProducerCompute::ServiceInfo> service_info_collection;
  for (vector<ProducerCompute::ServiceInfo>::iterator it = service_info_collection.begin(); it != service_info_collection.end(); ++it){
   if (it->service_name==service_name)
   {
     service_info_collection.push_back(*it);
   } 
  }
  return service_info_collection;
}


vector<ProducerCompute::ServiceInfo>
ProducerCompute::FetchFromServiceCollectionByStatus(ProducerCompute::ServiceStatus service_status)
{
  // TODO: improve this searching mechanism
  vector<ProducerCompute::ServiceInfo> service_info_collection;
  for (vector<ProducerCompute::ServiceInfo>::iterator it = service_info_collection.begin(); it != service_info_collection.end(); ++it){
   if (it->service_status==service_status)
   {
     service_info_collection.push_back(*it);
   } 
  }
  return service_info_collection;
}

ProducerCompute::ServiceInfo
ProducerCompute::UpdateServiceStatus(int service_id, ProducerCompute::ServiceStatus updated_service_status)
{
  
  ProducerCompute::ServiceInfo service_info=FetchFromServiceCollectionById(service_id);
  int service_index=GetServiceIndexInCollection(service_info.service_id);

  if(service_index!=-1)
  {
    service_info.service_status=updated_service_status; // set new status value
  
    service_info_collection.at(service_index)=service_info; // update in the list
      
  }
  return service_info; // TODO: return null in case of no update
}


int
ProducerCompute::GetServiceIndexInCollection(int service_id)
{
  int index=-1;
  auto  service_itr = find_if(service_info_collection.begin(), service_info_collection.end(), [&service_id](const ServiceInfo& x) {return x.service_id == service_id;});
  if(service_itr != service_info_collection.end())
  {
    index = distance(service_info_collection.begin(), service_itr);
  }
  return index;
}

int
ProducerCompute::CurrentTime()
{
  ndn::time::steady_clock::TimePoint now = ::ndn::time::steady_clock::now(); 
  ndn::time::milliseconds milliseconds = ::ndn::time::duration_cast<::ndn::time::milliseconds>(now.time_since_epoch());
  return milliseconds.count(); 
}

ns3::Ptr<ns3::Node> 
ProducerCompute::GetCurrentNode()
{
  ns3::Ptr<ns3::Node> currentNode;
  if (ns3::Simulator::GetContext() < 100000) 
  {
        currentNode = ns3::NodeList::GetNode(ns3::Simulator::GetContext());
  }
  return currentNode;
}

// Atif-Code: End


} // namespace ndn
} // namespace ns3
