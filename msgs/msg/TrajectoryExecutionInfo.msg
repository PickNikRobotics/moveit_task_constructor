# List of controllers to use when executing the trajectory
string[] controller_names
# MoveIt's PlanExecution capability that is used to execute MTC solutions has the ability to monitor a trajectory for collisions while it's being executed. This is useful in dynamic environments where obstacles may change between the time the plan was computed and its execution, or even during execution. If this flag is set to true, MoveIt's online collision monitor will be enabled, which can stop the trajectory at any point during execution if a collision is predicted.
bool trajectory_monitoring

