#ifndef DEVICE_DATA_H_
#define DEVICE_DATA_H_

#include "ident.h"

/* 0xffff is reserved here for an unregistered prototype vendor. */
#define OPENER_DEVICE_VENDOR_ID      (0xffff)
#define OPENER_DEVICE_TYPE           (12)
#define OPENER_DEVICE_PRODUCT_CODE   (1)
#define OPENER_DEVICE_MAJOR_REVISION (APP_IDENT_FW_VERSION_MAJOR)
#define OPENER_DEVICE_MINOR_REVISION (APP_IDENT_FW_VERSION_MINOR)
#define OPENER_DEVICE_NAME           (APP_IDENT_DEVICE_NAME)

#endif /* DEVICE_DATA_H_ */
