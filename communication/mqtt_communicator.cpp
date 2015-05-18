/*
 * This file is part of fast-lib.
 * Copyright (C) 2015 RWTH Aachen University - ACS
 *
 * This file is licensed under the GNU Lesser General Public License Version 3
 * Version 3, 29 June 2007. For details see 'LICENSE.md' in the root directory.
 */

#include "mqtt_communicator.hpp"

#include <stdexcept>
#include <cstdlib>
#include <thread>
#include <iostream>

namespace fast {

/// Helper function to make error codes readable.
std::string mosq_err_string(const std::string &str, int code)
{
	return str + mosqpp::strerror(code);
}

MQTT_communicator::MQTT_communicator(const std::string &id, 
				     const std::string &subscribe_topic,
				     const std::string &publish_topic,
				     const std::string &host, 
				     int port,
				     int keepalive) :
	mosqpp::mosquittopp(id.c_str()),
	id(id), 
	subscribe_topic(subscribe_topic),
	publish_topic(publish_topic),
	host(host), 
	port(port),
	keepalive(keepalive),
	connected(false)
{
	int ret;
	// Start threaded mosquitto loop
	if ((ret = loop_start()) != MOSQ_ERR_SUCCESS)
		throw std::runtime_error(mosq_err_string("Error starting mosquitto loop: ", ret));
	{	// Connect to MQTT broker. Uses condition variable that is set in on_connect, because
		// connect_async returning MOSQ_ERR_SUCCESS does not guarantee an established connection.
		std::unique_lock<std::mutex> lock(connected_mutex);
		if ((ret = connect_async(host.c_str(), port, keepalive)) != MOSQ_ERR_SUCCESS)
			throw std::runtime_error(mosq_err_string("Error connecting to MQTT broker: ", ret));
		while(!connected_cv.wait_for(lock, std::chrono::seconds(3), [this]{return connected;})) {
			if ((ret = reconnect_async()) != MOSQ_ERR_SUCCESS)
				throw std::runtime_error(mosq_err_string("Error connecting to MQTT broker: ", ret));
		}
	}
	// Subscribe to default topic
	subscribe(nullptr, subscribe_topic.c_str(), 2);
}

MQTT_communicator::MQTT_communicator(const std::string &id, 
				     const std::string &subscribe_topic,
				     const std::string &publish_topic,
				     const std::string &host, 
				     int port,
				     int keepalive,
				     const std::chrono::duration<double> &timeout) :
	mosqpp::mosquittopp(id.c_str()),
	id(id), 
	subscribe_topic(subscribe_topic),
	publish_topic(publish_topic),
	host(host), 
	port(port),
	keepalive(keepalive),
	connected(false)
{
	int ret;
	// Start threaded mosquitto loop
	if ((ret = loop_start()) != MOSQ_ERR_SUCCESS)
		throw std::runtime_error(mosq_err_string("Error starting mosquitto loop: ", ret));
	{ 	// Connect to MQTT broker. Uses condition variable that is set in on_connect, because
		// connect_async returning MOSQ_ERR_SUCCESS does not guarantee an established connection.
		auto start = std::chrono::high_resolution_clock::now();
		std::unique_lock<std::mutex> lock(connected_mutex);
		if ((ret = connect_async(host.c_str(), port, keepalive)) != MOSQ_ERR_SUCCESS)
			throw std::runtime_error(mosq_err_string("Error connecting to MQTT broker: ", ret));
		while(!connected_cv.wait_for(lock, std::chrono::seconds(3), [this]{return connected;})) {
			if (std::chrono::high_resolution_clock::now() - start > timeout)
				throw std::runtime_error("Timeout while trying to connect to MQTT broker.");
			if ((ret = reconnect_async() != MOSQ_ERR_SUCCESS))
				throw std::runtime_error(mosq_err_string("Error connecting to MQTT broker: ", ret));
		}
	}
	// Subscribe to default topic
	subscribe(nullptr, subscribe_topic.c_str(), 2);
}

MQTT_communicator::~MQTT_communicator()
{
	disconnect();
	loop_stop();
}

void MQTT_communicator::on_connect(int rc)
{
	if (rc == 0) {
		std::cout << "Connection established to " << host << ":" << port << std::endl;
		{
			std::lock_guard<std::mutex> lock(connected_mutex);
			connected = true;
		}
		connected_cv.notify_one();
	} else {
		std::cout << "Error on connect: " << mosqpp::connack_string(rc) << std::endl;
	}
}

void MQTT_communicator::on_disconnect(int rc)
{
	if (rc == 0) {
		std::cout << "Disconnected from  " << host << ":" << port << std::endl;
	} else {
		std::cout << mosq_err_string("Unexpected disconnect: ", rc) << std::endl;
	}
	std::lock_guard<std::mutex> lock(connected_mutex);
	connected = false;
}

void MQTT_communicator::on_message(const mosquitto_message *msg)
{
	mosquitto_message* buf = static_cast<mosquitto_message*>(malloc(sizeof(mosquitto_message)));
	if (!buf) {
		std::cout << "malloc failed allocating mosquitto_message." << std::endl;
	}
	std::lock_guard<std::mutex> lock(msg_queue_mutex);
	messages.push(buf);
	mosquitto_message_copy(messages.back(), msg);
	if (messages.size() == 1)
		msg_queue_empty_cv.notify_one();
}

void MQTT_communicator::send_message(const std::string &message)
{
	send_message(message, publish_topic);
}

void MQTT_communicator::send_message(const std::string &message, const std::string &topic)
{
	int ret = publish(nullptr, topic.c_str(), message.size(), message.c_str(), 2, false);
	if (ret != MOSQ_ERR_SUCCESS)
		throw std::runtime_error(mosq_err_string("Error sending message: ", ret));
}

std::string MQTT_communicator::get_message()
{
	std::unique_lock<std::mutex> lock(msg_queue_mutex);
	while (messages.empty())
		msg_queue_empty_cv.wait(lock);
	mosquitto_message *msg = messages.front();
	messages.pop();
	std::string buf(static_cast<char*>(msg->payload), msg->payloadlen);
	mosquitto_message_free(&msg);
	return buf;
}

std::string MQTT_communicator::get_message(const std::chrono::duration<double> &duration)
{
	std::unique_lock<std::mutex> lock(msg_queue_mutex);
	while (messages.empty())
		if (msg_queue_empty_cv.wait_for(lock, duration) == std::cv_status::timeout)
			throw std::runtime_error("Timeout while waiting for message.");
	mosquitto_message *msg = messages.front();
	messages.pop();
	std::string buf(static_cast<char*>(msg->payload), msg->payloadlen);
	mosquitto_message_free(&msg);
	return buf;
}

} // namespace fast
