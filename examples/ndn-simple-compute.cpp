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

// ndn-simple.cpp

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ndnSIM-module.h"

namespace ns3 {

/**
 * This scenario simulates a very simple network topology:
 *
 *
 *      +----------+     1Mbps      +--------+     1Mbps      +----------+
 *      | consumer | <------------> | router | <------------> | producer |
 *      +----------+         10ms   +--------+          10ms  +----------+
 *
 *
 * Consumer requests data from producer with frequency 10 interests per second
 * (interests contain constantly increasing sequence number).
 *
 * For every received interest, producer replies with a data packet, containing
 * 1024 bytes of virtual payload.
 *
 * To run scenario and see what is happening, use the following command:
 *
 *     NS_LOG=ndn.Consumer:ndn.Producer ./waf --run=ndn-simple
 */

int
main(int argc, char* argv[])
{
  // setting default parameters for PointToPoint links and channels
  Config::SetDefault("ns3::PointToPointNetDevice::DataRate", StringValue("1Mbps"));
  Config::SetDefault("ns3::PointToPointChannel::Delay", StringValue("10ms"));
  Config::SetDefault("ns3::QueueBase::MaxSize", StringValue("20p"));

  // Read optional command-line parameters (e.g., enable visualizer with ./waf --run=<> --visualize
  CommandLine cmd;
  cmd.Parse(argc, argv);

  // Creating nodes
  NodeContainer nodes;
  nodes.Create(4);

  // Connecting nodes using two links
  PointToPointHelper p2p;
  p2p.Install(nodes.Get(0), nodes.Get(2));
  p2p.Install(nodes.Get(1), nodes.Get(2));
  p2p.Install(nodes.Get(2), nodes.Get(3));


  //Atif-Code: adding node resources capacity - Start

  // Setting up nodes resources and capacity (randomly)
  ns3::Ptr<ns3::UniformRandomVariable> m_rand(ns3::CreateObject<ns3::UniformRandomVariable>());
  for (size_t i = 0; i < nodes.GetN(); i++)
  {
    nodes.Get(i)->m_initial_compute_capacity=100.0; // in percentage
    nodes.Get(i)->m_initial_storage_capacity=100.0; // in percentage
    nodes.Get(i)->m_compute_capacity=100.0; // in percentage
    nodes.Get(i)->m_storage_capacity=100.0; // in percentage

    nodes.Get(i)->m_inital_compute_resources=m_rand->GetValue(1000, 10000); // in % in GHZ/S
    nodes.Get(i)->m_inital_storage_resources=m_rand->GetValue(1000, 10000); // in % GB
    nodes.Get(i)->m_compute_resources=nodes.Get(i)->m_inital_compute_resources; // in % in GHZ/S
    nodes.Get(i)->m_storage_resources=nodes.Get(i)->m_inital_storage_resources; // in % GB

    // printing the values
    // std::cout<<"Nodes Initial Compute Capacity is: "<<nodes.Get(i)->m_initial_compute_capacity<<std::endl;
    // std::cout<<"Nodes Initial Storage Capacity is: "<<nodes.Get(i)->m_initial_compute_capacity<<std::endl;
    // std::cout<<"Nodes Initial Compute Resources is: "<<nodes.Get(i)->m_inital_compute_resources<<std::endl;
    // std::cout<<"Nodes Initial Storage Resoruces is: "<<nodes.Get(i)->m_inital_storage_resources<<std::endl;

  }

  //Atif-Code: adding node resources capacity - End


  // Install NDN stack on all nodes
  ndn::StackHelper ndnHelper;
  ndnHelper.SetDefaultRoutes(true);
  ndnHelper.InstallAll();

  // Choosing forwarding strategy
  ndn::StrategyChoiceHelper::InstallAll("/prefix", "/localhost/nfd/strategy/multicast");

  // Installing applications

  // Consumer
  ndn::AppHelper consumerHelper("ns3::ndn::ConsumerComputeTraffic");
  // Consumer service name is being set in ConsumerCompute class
  //consumerHelper.SetPrefix("/prefix/"); // Prefixservice name: service_1, type: 1, priority: 1
  consumerHelper.SetAttribute("Frequency", StringValue("1")); // 1 interests a second
  auto apps = consumerHelper.Install(nodes.Get(0));                        // first node
  // apps.Stop(Seconds(20.0)); // stop the consumer app at 10 seconds mark

  consumerHelper.SetAttribute("Frequency", StringValue("1")); // 1 interests a second
  auto consumer_app_2 = consumerHelper.Install(nodes.Get(1));                        // first node
  consumer_app_2.Start(Seconds(5.0)); // start app after 5 seconds of simulations start
  // consumer_app_2.Stop(Seconds(19.0)); // stop the consumer app at 15 seconds mark

  // Producer
  ndn::AppHelper producerHelper("ns3::ndn::ProducerCompute");
  // Producer will reply to all requests starting with /prefix
  producerHelper.SetPrefix("/prefix/");
  producerHelper.SetAttribute("PayloadSize", StringValue("1024"));
  producerHelper.Install(nodes.Get(3)); // last node

  Simulator::Stop(Seconds(60.0));

  Simulator::Run();
  Simulator::Destroy();

  return 0;
}

} // namespace ns3

int
main(int argc, char* argv[])
{
  return ns3::main(argc, argv);
}
