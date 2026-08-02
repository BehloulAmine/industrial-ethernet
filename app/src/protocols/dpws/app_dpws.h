#ifndef APP_DPWS_H_
#define APP_DPWS_H_

#include <stdbool.h>
#include <stddef.h>

#define APP_DPWS_METADATA_XML_SIZE 4096

int app_dpws_start(void);
bool app_dpws_is_metadata_path(const char *path);
int app_dpws_build_metadata(char *xml, size_t xml_size, const char *request);

#endif /* APP_DPWS_H_ */
