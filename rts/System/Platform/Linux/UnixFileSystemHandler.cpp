/**
 * @file UnixFileSystemHandler.cpp
 * @brief Abstracts locating of content on different platforms
 * @author Tobi Vollebregt
 *
 * Unix implementation, supporting multiple data directories / search paths.
 *
 * Copyright (C) 2006-2008 Tobi Vollebregt.
 * Licensed under the terms of the GNU GPL, v2 or later
 */

#include "StdAfx.h"
#include "UnixFileSystemHandler.h"
#include <boost/regex.hpp>
#include <dirent.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "System/FileSystem/ArchiveScanner.h"
#include "System/FileSystem/VFSHandler.h"
#include "System/LogOutput.h"
#include "System/Platform/ConfigHandler.h"
#include "mmgr.h"

/**
 * @brief Creates the archive scanner and vfs handler
 *
 * For the archiveScanner, it keeps cache data ("archivecache.txt") in the
 * writedir. Cache data in other directories is ignored.
 * It scans maps, base and mods subdirectories of all readable datadirs
 * for archives. Archives in higher priority datadirs override archives
 * in lower priority datadirs.
 *
 * Note that the archive namespace is global, ie. each archive basename may
 * only occur once in all data directories. If a name is found more then once,
 * then higher priority datadirs override lower priority datadirs and per
 * datadir the 'mods' subdir overrides 'base' which overrides 'maps'.
 */
void UnixFileSystemHandler::InitVFS() const
{
	const DataDir* writedir = locater.GetWriteDir();
	const std::vector<DataDir>& datadirs = locater.GetDataDirs();

	archiveScanner = new CArchiveScanner();
	archiveScanner->ReadCacheData(writedir->path + archiveScanner->GetFilename());
	for (std::vector<DataDir>::const_reverse_iterator d = datadirs.rbegin(); d != datadirs.rend(); ++d) {
		archiveScanner->Scan(d->path + "maps", true);
		archiveScanner->Scan(d->path + "base", true);
		archiveScanner->Scan(d->path + "mods", true);
	}
	archiveScanner->WriteCacheData(writedir->path + archiveScanner->GetFilename());

	hpiHandler = new CVFSHandler();
}

/**
 * @brief Creates the archive scanner and vfs handler
 *
 * Locates data directories and initializes the VFS.
 */
UnixFileSystemHandler::UnixFileSystemHandler(bool verbose, bool initialize)
{
	if(initialize){
		locater.LocateDataDirs();
		InitVFS();
	}
}


UnixFileSystemHandler::~UnixFileSystemHandler()
{
	configHandler.Deallocate();
}

/**
 * @brief returns the highest priority writable directory, aka the writedir
 */
std::string UnixFileSystemHandler::GetWriteDir() const
{
	const DataDir* writedir = locater.GetWriteDir();
	assert(writedir && writedir->writable); //duh
	return writedir->path;
}

/**
 * @brief find files
 * @param dir path in which to start looking (tried relative to each data directory)
 * @param pattern pattern to search for
 * @param recurse whether or not to recursively search
 * @param include_dirs whether or not to include directory names in the result
 * @return vector of std::strings containing absolute paths to the files
 *
 * Will search for a file given a particular pattern.
 * Starts from dirpath, descending down if recurse is true.
 */
std::vector<std::string> UnixFileSystemHandler::FindFiles(const std::string& dir, const std::string& pattern, int flags) const
{
	std::vector<std::string> matches;

	// if it's an absolute path, don't look for it in the data directories
	if (dir[0] == '/') {
		FindFilesSingleDir(matches, dir, pattern, flags);
		return matches;
	}

	const std::vector<DataDir>& datadirs = locater.GetDataDirs();
	for (std::vector<DataDir>::const_reverse_iterator d = datadirs.rbegin(); d != datadirs.rend(); ++d) {
		FindFilesSingleDir(matches, d->path + dir, pattern, flags);
	}
	return matches;
}

std::string UnixFileSystemHandler::LocateFile(const std::string& file) const
{
	// if it's an absolute path, don't look for it in the data directories
	if (file[0] == '/')
		return file;

	const std::vector<DataDir>& datadirs = locater.GetDataDirs();
	for (std::vector<DataDir>::const_iterator d = datadirs.begin(); d != datadirs.end(); ++d) {
		std::string fn(d->path + file);
		if (access(fn.c_str(), R_OK | F_OK) == 0)
			return fn;
	}
	return file;
}

std::vector<std::string> UnixFileSystemHandler::GetDataDirectories() const
{
	std::vector<std::string> f;

	const std::vector<DataDir>& datadirs = locater.GetDataDirs();
	for (std::vector<DataDir>::const_iterator d = datadirs.begin(); d != datadirs.end(); ++d) {
		f.push_back(d->path);
	}
	return f;
}

/**
 * @brief creates a rwxr-xr-x dir in the writedir
 *
 * Returns true if the postcondition of this function is that dir exists in
 * the write directory.
 *
 * Note that this function does not check access to the dir, ie. if you've
 * created it manually with 0000 permissions then this function may return
 * true, subsequent operation on files inside the directory may still fail.
 *
 * As a rule of thumb, set identical permissions on identical items in the
 * data directory, ie. all subdirectories the same perms, all files the same
 * perms.
 */
bool UnixFileSystemHandler::mkdir(const std::string& dir) const
{
	struct stat info;

	// First check if directory exists. We'll return success if it does.
	if (stat(dir.c_str(), &info) == 0 && S_ISDIR(info.st_mode))
		return true;

	// If it doesn't exist we try to mkdir it and return success if that succeeds.
	if (::mkdir(dir.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) == 0)
		return true;

	// Otherwise we return false.
	return false;
}

static void FindFiles(std::vector<std::string>& matches, const std::string& dir, const boost::regex &regexpattern, int flags)
{
	DIR* dp;
	struct dirent* ep;

	if (!(dp = opendir(dir.c_str())))
		return;

	while ((ep = readdir(dp))) {
		// exclude hidden files
		if (ep->d_name[0] != '.') {
			// is it a file? (we just treat sockets / pipes / fifos / character&block devices as files...)
			// (need to stat because d_type is DT_UNKNOWN on linux :-/)
			struct stat info;
			if (stat((dir + ep->d_name).c_str(), &info) == 0) {
				if (!S_ISDIR(info.st_mode)) {
					if ((flags & FileSystem::ONLY_DIRS) == 0) {
						if (boost::regex_match(ep->d_name, regexpattern)) {
							matches.push_back(dir + ep->d_name);
						}
					}
				}
				else {
					// or a directory?
					if (flags & FileSystem::INCLUDE_DIRS) {
						if (boost::regex_match(ep->d_name, regexpattern)) {
							matches.push_back(dir + ep->d_name + '/');
						}
					}
					if (flags & FileSystem::RECURSE) {
						FindFiles(matches, dir + ep->d_name + '/', regexpattern, flags);
					}
				}
			}
		}
	}
	closedir(dp);
}


/**
 * @brief internal find-files-in-a-single-datadir-function
 * @param dir path in which to start looking
 * @param pattern pattern to search for
 * @param recurse whether or not to recursively search
 * @param include_dirs whether or not to include directory names in the result
 * @return vector of std::strings
 *
 * Will search for a file given a particular pattern.
 * Starts from dirpath, descending down if recurse is true.
 */
void UnixFileSystemHandler::FindFilesSingleDir(std::vector<std::string>& matches, const std::string& dir, const std::string &pattern, int flags) const
{
	assert(!dir.empty() && dir[dir.length() - 1] == '/');

	boost::regex regexpattern(filesystem.glob_to_regex(pattern));

	::FindFiles(matches, dir, regexpattern, flags);
}
