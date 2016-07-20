Module adds statistic report about memory contexts in local and all backends.

Functions:

setof (name text, level integer, nblocks bigint, freechunks bigint,
	   totalspace bigint, freespace bigint)
	local_memory_stats()
	prints memory context's statistic for current backend

setof (pid integer, name text, level integer, nblocks bigint, freechunks bigint,
	   totalspace bigint, freespace bigint)
	instance_memory_stats()
	prints memory context's statistic for all alive backend, works if library
	was preloaded via shared_preload_libraries.

view memory_stats
	prints per backend summary memory statistics 	

To use instance_memory_stats() it's needed to add memstat library to
shared_preload_libraries. And it should be last in that list!

GUC variable:
	memstat.period = 10 # seconds

	Module collects memory statistics at a begining of each query and
	it could be expensive on highloaded instances, so, this variable
	set minimal time between statistic obtaining.

Copyright (c) 2016, Teodor Sigaev <teodor@sigaev.ru>
