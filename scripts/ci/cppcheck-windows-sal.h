#pragma once

// Cppcheck parses the MSVC project without the Windows SDK preprocessor.
// Define only the two SAL annotations used by the tracked core sources.
#ifndef __out_ecount_part_opt
#define __out_ecount_part_opt(size, length)
#endif

#ifndef _In_reads_bytes_
#define _In_reads_bytes_(size)
#endif

#ifndef _Out_writes_bytes_to_opt_
#define _Out_writes_bytes_to_opt_(size, length)
#endif

#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(size)
#endif
