/*
 * Copyright (C) 2014 GRNET S.A.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef LOGGER_HH
#define LOGGER_HH

#include <log4cplus/logger.h>

namespace archipelago {

class Logger: public log4cplus::Logger {
public:
    Logger(const std::string& conffile, const std::string& instance);

    void logerror(const std::string& msg);
    void logfatal(const std::string& msg);
    void loginfo(const std::string& msg);
    void logdebug(const std::string& msg);
    void logwarn(const std::string& msg);
    void logtrace(const std::string& msg);
private:
    log4cplus::Logger logger;
    void logGeneric(int loglevel, const std::string& msg);
};

}

#endif