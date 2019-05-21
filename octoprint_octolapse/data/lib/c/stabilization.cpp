////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Octolapse - A plugin for OctoPrint used for making stabilized timelapse videos.
// Copyright(C) 2019  Brad Hochgesang
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// This program is free software : you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.If not, see the following :
// https ://github.com/FormerLurker/Octolapse/blob/master/LICENSE
//
// You can contact the author either through the git - hub repository, or at the
// following email address : FormerLurker@pm.me
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#include "stabilization.h"
#include <fstream>
#include <time.h>
#include <iostream>
#include <vector>
#include "logging.h"



stabilization::stabilization(
	gcode_position_args* position_args, stabilization_args* stab_args, pythonProgressCallback progress
)
{
	std::string errors_;
	if (stab_args->py_on_progress_received != NULL)
	{
		python_callbacks_ = true;
		python_progress_callback_ = stab_args->py_on_progress_received;
	}
	else
	{
		python_callbacks_ = false;
		python_progress_callback_ = NULL;
	}
	progress_callback_ = progress;
	native_progress_callback_ = NULL;
	p_stabilization_args_ = stab_args;
	is_running_ = true;
	gcode_parser_ = new gcode_parser();
	gcode_position_ = new gcode_position(position_args);
	file_size_ = 0;
	lines_processed_ = 0;
	gcodes_processed_ = 0;
	update_period_seconds_ = stab_args->notification_period_seconds_;
}

stabilization::stabilization()
{
	std::string errors_;
	python_callbacks_ = false;
	native_progress_callback_ = NULL;
	python_progress_callback_ = NULL;
	progress_callback_ = NULL;
	p_stabilization_args_ = NULL;
	is_running_ = true;
	gcode_parser_ = NULL;
	gcode_position_ = NULL;
	file_size_ = 0;
	lines_processed_ = 0;
	gcodes_processed_ = 0;
	update_period_seconds_ = 0.25;
}

stabilization::stabilization(gcode_position_args* position_args, stabilization_args* args, progressCallback progress)
{
	std::string errors_; 
	python_callbacks_ = false;
	native_progress_callback_ = progress;
	python_progress_callback_ = NULL;
	progress_callback_ = NULL;
	p_stabilization_args_ = args;
	is_running_ = true;
	gcode_parser_ = new gcode_parser();
	gcode_position_ = new gcode_position(position_args);
	file_size_ = 0;
	lines_processed_ = 0;
	gcodes_processed_ = 0;
	update_period_seconds_ = args->notification_period_seconds_;
}

stabilization::stabilization(const stabilization &source)
{
	// Private copy constructor, don't copy me!	
}

stabilization::~stabilization()
{
	if (gcode_parser_ != NULL)
	{
		delete gcode_parser_;
		gcode_parser_ = NULL;
	}
	if (gcode_position_ != NULL)
	{
		delete gcode_position_;
		gcode_position_ = NULL;
	}
	
}

long stabilization::get_file_size(const std::string& file_path)
{
	// Todo:  Fix this function.  This is a pretty weak implementation :(
	std::ifstream file(file_path.c_str(), std::ios::in | std::ios::binary);
	const long l = (long)file.tellg();
	file.seekg(0, std::ios::end);
	const long m = (long)file.tellg();
	file.close();
	return (m - l);
}

double stabilization::get_next_update_time() const
{
	return clock() + (update_period_seconds_ * CLOCKS_PER_SEC);
}

double stabilization::get_time_elapsed(double start_clock, double end_clock)
{
	return static_cast<double>(end_clock - start_clock) / CLOCKS_PER_SEC;
}

void stabilization::process_file(stabilization_results* results)
{
	
	p_snapshot_plans_ = &results->snapshot_plans_;
	//std::cout << "stabilization::process_file - Processing file.\r\n";
	octolapse_log(SNAPSHOT_PLAN, INFO, "Processing File.");
	PyThreadState *_save = NULL;
	is_running_ = true;
	
	double next_update_time = get_next_update_time();
	const clock_t start_clock = clock();

	// todo : clear out everything for a fresh go!
	file_size_ = get_file_size(p_stabilization_args_->file_path_);
	std::ifstream gcodeFile(p_stabilization_args_->file_path_.c_str());
	std::string line;

	if (gcodeFile.is_open())
	{
		// Communicate every second
		parsed_command* cmd = new parsed_command();
		while (std::getline(gcodeFile, line) && is_running_)
		{
			lines_processed_++;
			cmd->clear();
			//std::cout << "stabilization::process_file - parsing gcode: " << line << "...";
			gcode_parser_->try_parse_gcode(line.c_str(), cmd);
			//std::cout << "Complete.\r\n";
			if (!cmd->cmd_.empty())
			{

				gcodes_processed_++;
				//std::cout << "stabilization::process_file - updating position...";
				gcode_position_->update(cmd, lines_processed_, gcodes_processed_);
				//std::cout << "Complete.\r\n";
				process_pos(gcode_position_->get_current_position(), gcode_position_->get_previous_position());
				if (next_update_time < clock())
				{
					// ToDo: tellg does not do what I think it does, but why?
					long currentPosition = (long)gcodeFile.tellg();
					long bytesRemaining = file_size_ - currentPosition;
					double percentProgress = (double)currentPosition / (double)file_size_*100.0;
					double secondsElapsed = get_time_elapsed(start_clock, clock());
					double bytesPerSecond = (double)currentPosition / secondsElapsed;
					double secondsToComplete = bytesRemaining / bytesPerSecond;
					//std::cout << "stabilization::process_file - notifying progress...";
					notify_progress(percentProgress, secondsElapsed, secondsToComplete, gcodes_processed_,
						lines_processed_);
					//std::cout << "Complete.\r\n";
					next_update_time = get_next_update_time();
				}

			}
		}
		// deallocate the parsed_command object
		
		gcodeFile.close();
		on_processing_complete();
		delete cmd;
		//std::cout << "stabilization::process_file - Completed Processing file.\r\n";
	}

	const clock_t end_clock = clock();
	const double total_seconds = static_cast<double>(end_clock - start_clock) / CLOCKS_PER_SEC;
	results->success_ = errors_.empty();
	results->errors_ = errors_;
	results->seconds_elapsed_ = total_seconds;
	results->gcodes_processed_ = gcodes_processed_;
	results->lines_processed_ = lines_processed_;
	octolapse_log(SNAPSHOT_PLAN, INFO, "Completed file processing.");
	p_snapshot_plans_ = NULL;
}

double stabilization::get_carteisan_distance(double x1, double y1, double x2, double y2)
{
	// Compare the saved points cartesian distance from the current point
	double xdif = x1 - x2;
	double ydif = y1 - y2;
	double dist_squared = xdif * xdif + ydif * ydif;
	if (dist_squared < 0)
	{
		//std::cout << "Negative Distance found!  Returning -1\r\n";
		return -1.0;
	}
	return sqrt(xdif*xdif + ydif * ydif);
}

void stabilization::notify_progress(const double percent_progress, const double seconds_elapsed, const double seconds_to_complete,
	const long gcodes_processed, const long lines_processed)
{
	if (python_callbacks_)
	{
		is_running_ = progress_callback_(python_progress_callback_, percent_progress, seconds_elapsed, seconds_to_complete, gcodes_processed, lines_processed);
	}
	else if(native_progress_callback_ != NULL)
	{
		is_running_ = native_progress_callback_(percent_progress, seconds_elapsed, seconds_to_complete, gcodes_processed, lines_processed);
	}

}

void stabilization::process_pos(position* current_pos, position* previous_pos)
{
	throw std::exception();
}
void stabilization::on_processing_complete()
{
	throw std::exception();
}



stabilization_args::stabilization_args() 
{
	stabilization_type_ = "";
	is_bound_ = false;
	x_min_ = 0;
	x_max_ = 0;
	y_min_ = 0;
	y_max_ = 0;
	z_min_ = 0;
	z_max_ = 0;
	height_increment_ = 0.0;
	notification_period_seconds_ = 0.25;
	fastest_speed_ = true;
	file_path_ = "";
};

stabilization_args::~stabilization_args()
{

}
