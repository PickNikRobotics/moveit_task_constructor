/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2017, Bielefeld University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Bielefeld University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Robert Haschke
	Desc:   Monitor manipulation tasks and visualize their solutions
*/

#include "task_display.h"
#include "task_list_model_cache.h"
#include <moveit_task_constructor/introspection.h>
#include <moveit_task_constructor/visualization_tools/task_solution_visualization.h>

#include <moveit/rdf_loader/rdf_loader.h>
#include <moveit/robot_model/robot_model.h>

#include <rviz/properties/string_property.h>
#include <rviz/properties/ros_topic_property.h>
#include <rviz/properties/status_property.h>

namespace moveit_rviz_plugin
{

TaskDisplay::TaskDisplay() : Display()
{
	robot_description_property_ =
	      new rviz::StringProperty("Robot Description", "robot_description", "The name of the ROS parameter where the URDF for the robot is loaded",
	                               this, SLOT(changedRobotDescription()), this);

	task_solution_topic_property_ =
	      new rviz::RosTopicProperty("Task Solution Topic", "",
	                                 ros::message_traits::datatype<moveit_task_constructor::Solution>(),
	                                 "The topic on which task solutions (moveit_msgs::Solution messages) are received",
	                                 this, SLOT(changedTaskSolutionTopic()), this);

	trajectory_visual_.reset(new TaskSolutionVisualization(this, this));
	tasks_property_ =
	      new rviz::Property("Tasks", QVariant(), "Tasks received on monitored topic", this);
}

void TaskDisplay::onInitialize()
{
	Display::onInitialize();
	trajectory_visual_->onInitialize(scene_node_, context_);
}

void TaskDisplay::loadRobotModel()
{
	rdf_loader_.reset(new rdf_loader::RDFLoader(robot_description_property_->getStdString()));

	if (!rdf_loader_->getURDF())
	{
		this->setStatus(rviz::StatusProperty::Error, "Robot Model",
		                "Failed to load from parameter " + robot_description_property_->getString());
		return;
	}
	this->setStatus(rviz::StatusProperty::Ok, "Robot Model", "Successfully loaded");

	const srdf::ModelSharedPtr& srdf =
	      rdf_loader_->getSRDF() ? rdf_loader_->getSRDF() : srdf::ModelSharedPtr(new srdf::Model());
	robot_model_.reset(new robot_model::RobotModel(rdf_loader_->getURDF(), srdf));

	// Send to child class
	trajectory_visual_->onRobotModelLoaded(robot_model_);
	trajectory_visual_->onEnable();
}

void TaskDisplay::reset()
{
	Display::reset();
	loadRobotModel();
	trajectory_visual_->reset();
}

void TaskDisplay::onEnable()
{
	Display::onEnable();
	loadRobotModel();

	// (re)initialize task model
	updateTaskListModel();
}

void TaskDisplay::onDisable()
{
	Display::onDisable();
	trajectory_visual_->onDisable();

	// don't monitor topics when disabled
	task_description_sub.shutdown();
	task_statistics_sub.shutdown();
	task_solution_sub.shutdown();
}

void TaskDisplay::update(float wall_dt, float ros_dt)
{
	Display::update(wall_dt, ros_dt);
	mainloop_jobs_.executeJobs();
	trajectory_visual_->update(wall_dt, ros_dt);
}

void TaskDisplay::setName(const QString& name)
{
	BoolProperty::setName(name);
	trajectory_visual_->setName(name);
}

void TaskDisplay::changedRobotDescription()
{
	if (isEnabled())
		reset();
	else
		loadRobotModel();
}

void TaskDisplay::taskDescriptionCB(const ros::MessageEvent<const moveit_task_constructor::TaskDescription> &event)
{
	const moveit_task_constructor::TaskDescriptionConstPtr& msg = event.getMessage();
	const std::string id = event.getPublisherName() + "/" + msg->id;
	mainloop_jobs_.addJob([this, id, msg]() {
		task_list_model_->processTaskDescriptionMessage(id, *msg);
	});
}

void TaskDisplay::taskStatisticsCB(const ros::MessageEvent<const moveit_task_constructor::TaskStatistics> &event)
{
	const moveit_task_constructor::TaskStatisticsConstPtr& msg = event.getMessage();
	const std::string id = event.getPublisherName() + "/" + msg->id;
	mainloop_jobs_.addJob([this, id, msg]() {
		task_list_model_->processTaskStatisticsMessage(id, *msg);
	});
}

void TaskDisplay::taskSolutionCB(const ros::MessageEvent<const moveit_task_constructor::Solution> &event)
{
	const moveit_task_constructor::SolutionConstPtr& msg = event.getMessage();
	const std::string id = event.getPublisherName() + "/" + msg->task_id;
	mainloop_jobs_.addJob([this, id, msg]() {
		if (task_list_model_) task_list_model_->processSolutionMessage(id, *msg);
		// TODO: use already processed trajectory (e.g. by ID)
		trajectory_visual_->showTrajectory(*msg);
	});
}

void TaskDisplay::updateTaskListModel()
{
	if (task_list_model_) {
		disconnect(task_list_model_.get(), &TaskListModel::rowsInserted, this, &TaskDisplay::onTasksInserted);
		disconnect(task_list_model_.get(), &TaskListModel::rowsAboutToBeRemoved, this, &TaskDisplay::onTasksRemoved);
	}
	tasks_property_->removeChildren();

	// generate task monitoring topics from solution topic
	std::string solution_topic = task_solution_topic_property_->getStdString();
	auto last_sep = solution_topic.find_last_of('/');
	if (last_sep == std::string::npos) {
		setStatus(rviz::StatusProperty::Error, "Task Monitor", "invalid topic");
		return;
	}

	std::string base_ns = solution_topic.substr(0, last_sep+1);
	task_list_model_ = TaskListModelCache::instance().getModel(base_ns);

	if (task_list_model_) {
		// listen to task descriptions updates
		task_description_sub = update_nh_.subscribe(base_ns + DESCRIPTION_TOPIC, 2, &TaskDisplay::taskDescriptionCB, this);

		// listen to task statistics updates
		task_statistics_sub = update_nh_.subscribe(base_ns + STATISTICS_TOPIC, 2, &TaskDisplay::taskStatisticsCB, this);

		setStatus(rviz::StatusProperty::Ok, "Task Monitor", "Connected");

		// initialize task list
		onTasksInserted(QModelIndex(), 0, task_list_model_->rowCount()-1);
		connect(task_list_model_.get(), &TaskListModel::rowsInserted, this, &TaskDisplay::onTasksInserted);
		connect(task_list_model_.get(), &TaskListModel::rowsAboutToBeRemoved, this, &TaskDisplay::onTasksRemoved);
		connect(task_list_model_.get(), &TaskListModel::dataChanged, this, &TaskDisplay::onTaskDataChanged);
	} else {
		setStatus(rviz::StatusProperty::Error, "Task Monitor", "failed to create TaskListModel");
	}

	// listen to task solutions
	task_solution_sub = update_nh_.subscribe(solution_topic, 2, &TaskDisplay::taskSolutionCB, this);
}

void TaskDisplay::changedTaskSolutionTopic()
{
	task_description_sub.shutdown();
	task_statistics_sub.shutdown();
	task_solution_sub.shutdown();
	updateTaskListModel();
}

void TaskDisplay::onTasksInserted(const QModelIndex &parent, int first, int last)
{
	if (parent.isValid()) return; // only handle top-level items

	TaskListModel* m = static_cast<TaskListModel*>(sender());
	for (; first <= last; ++first) {
		QModelIndex idx = m->index(first, 0, parent);
		tasks_property_->addChild(new rviz::Property(idx.data().toString(), idx.sibling(idx.row(), 1).data()));
	}
}

void TaskDisplay::onTasksRemoved(const QModelIndex &parent, int first, int last)
{
	if (parent.isValid()) return; // only handle top-level items

	for (; first <= last; ++first) {
		rviz::Property *child = tasks_property_->takeChildAt(first);
		delete child;
	}
}

void TaskDisplay::onTaskDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight)
{
	if (topLeft.parent().isValid()) return; // only handle top-level items

	for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
		rviz::Property *child = tasks_property_->childAt(row);
		if (topLeft.column() <= 0 && 0 <= bottomRight.column()) // name changed
			child->setName(topLeft.sibling(row, 0).data().toString());
		if (topLeft.column() <= 1 && 1 <= bottomRight.column()) // #solutions changed
			child->setValue(topLeft.sibling(row, 1).data());
	}
}

}  // namespace moveit_rviz_plugin