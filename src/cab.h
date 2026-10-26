/* pcapload - extract PopCap game .cab installers via cabinet.dll (FDI) */
#ifndef PCAPLOAD_CAB_H
#define PCAPLOAD_CAB_H

/* Extract all files from cabPath into destDir (created if needed).
   Returns 0 on success, non-zero on failure. */
int cab_extract(const char* cabPath, const char* destDir);

#endif
