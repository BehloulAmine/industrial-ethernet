/* OpENer application integration for the Zephyr EtherNet/IP adapter. */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include "app_eip.h"
#include "app_modbus_scanner.h"
#include "appcontype.h"
#include "cipconnectionobject.h"
#include "cipethernetlink.h"
#include "cipidentity.h"
#include "cipqos.h"
#include "cipstring.h"
#include "ciptcpipinterface.h"
#include "doublylinkedlist.h"
#include "generic_networkhandler.h"
#include "net_cfg.h"
#include "opener_api.h"

LOG_MODULE_REGISTER(app_eip, LOG_LEVEL_INF);

#define APP_EIP_STACK_SIZE 12288
#define APP_EIP_THREAD_PRIORITY 7
#define APP_EIP_ASSEMBLY_SIZE (APP_EIP_ASSEMBLY_WORD_COUNT * sizeof(uint16_t))
#define APP_EIP_SERIAL_NUMBER 0x07470001U
#define APP_EIP_PRODUCT_NAME "STM32H747 Industrial Demo"
#define APP_EIP_HOST_NAME "industrial-ethernet"
#define APP_EIP_HEAP_SIZE 32768

K_THREAD_STACK_DEFINE(eip_stack, APP_EIP_STACK_SIZE);
K_HEAP_DEFINE(eip_heap, APP_EIP_HEAP_SIZE);

static struct k_thread eip_thread;
static uint8_t input_assembly[APP_EIP_ASSEMBLY_SIZE];
static uint8_t output_assembly[APP_EIP_ASSEMBLY_SIZE];
static atomic_t io_connection_count;
static bool eip_started;

static void eip_reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_reboot(SYS_REBOOT_COLD);
}

K_WORK_DELAYABLE_DEFINE(eip_reboot_work, eip_reboot_work_handler);

static int wait_for_ipv4(struct net_cfg_data *active)
{
	struct in_addr address;

	while (true) {
		if (net_cfg_get_active(active) == 0 &&
		    net_addr_pton(AF_INET, active->ip, &address) == 0 &&
		    address.s_addr != 0U) {
			return 0;
		}
		k_msleep(100);
	}
}

static int configure_cip_network(const struct net_cfg_data *active)
{
	struct in_addr address;

	if (net_addr_pton(AF_INET, active->ip, &address) < 0) {
		return -EINVAL;
	}
	g_tcpip.interface_configuration.ip_address = address.s_addr;

	if (net_addr_pton(AF_INET, active->mask, &address) < 0) {
		return -EINVAL;
	}
	g_tcpip.interface_configuration.network_mask = address.s_addr;

	if (net_addr_pton(AF_INET, active->gw, &address) < 0) {
		return -EINVAL;
	}
	g_tcpip.interface_configuration.gateway = address.s_addr;
	g_tcpip.config_control = active->mode == NET_CFG_DHCP ?
		kTcpipCfgCtrlDhcp : kTcpipCfgCtrlStaticIp;
	(void)SetCipStringByCstr(&g_tcpip.hostname, APP_EIP_HOST_NAME);
	return 0;
}

static void configure_cip_identity(void)
{
	struct net_if *iface = net_if_get_default();
	const struct net_linkaddr *link_address =
		iface != NULL ? net_if_get_link_addr(iface) : NULL;
	uint8_t mac[6];
	uint32_t serial = APP_EIP_SERIAL_NUMBER;

	if (link_address != NULL && link_address->len >= 6U) {
		serial = sys_get_be32(&link_address->addr[2]);
		memcpy(mac, link_address->addr, sizeof(mac));
		CipEthernetLinkSetMac(mac);
	}

	SetDeviceSerialNumber(serial);
	SetDeviceProductName(APP_EIP_PRODUCT_NAME);
}

static void eip_thread_fn(void *arg1, void *arg2, void *arg3)
{
	struct net_cfg_data active;
	EipStatus status;
	uint16_t unique_connection_id;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	(void)wait_for_ipv4(&active);
	DoublyLinkedListInitialize(&connection_list,
				   CipConnectionObjectListArrayAllocator,
				   CipConnectionObjectListArrayFree);

	unique_connection_id = (uint16_t)(k_cycle_get_32() ^ APP_EIP_SERIAL_NUMBER);
	status = CipStackInit(unique_connection_id);
	if (status != kEipStatusOk) {
		LOG_ERR("OpENer CIP initialization failed: %d", status);
		return;
	}

	configure_cip_identity();
	if (configure_cip_network(&active) < 0) {
		LOG_ERR("OpENer network configuration failed");
		ShutdownCipStack();
		return;
	}

	status = NetworkHandlerInitialize();
	if (status != kEipStatusOk) {
		LOG_ERR("OpENer socket initialization failed: %d", status);
		ShutdownCipStack();
		return;
	}

	LOG_INF("OpENer ready: TCP/UDP 44818, I/O UDP 2222, output=%u input=%u",
		APP_EIP_OUTPUT_ASSEMBLY, APP_EIP_INPUT_ASSEMBLY);
	while (NetworkHandlerProcessCyclic() == kEipStatusOk) {
		HandleApplication();
	}

	LOG_ERR("OpENer network loop stopped");
	(void)NetworkHandlerFinish();
	ShutdownCipStack();
}

int app_eip_start(void)
{
	if (eip_started) {
		return 0;
	}

	(void)k_thread_create(&eip_thread, eip_stack,
			      K_THREAD_STACK_SIZEOF(eip_stack), eip_thread_fn,
			      NULL, NULL, NULL, APP_EIP_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&eip_thread, "opener");
	eip_started = true;
	return 0;
}

uint16_t app_eip_connection_count(void)
{
	return (uint16_t)atomic_get(&io_connection_count);
}

EipStatus ApplicationInitialization(void)
{
	if (CreateAssemblyObject(APP_EIP_CONFIG_ASSEMBLY, NULL, 0) == NULL ||
	    CreateAssemblyObject(APP_EIP_INPUT_ASSEMBLY, input_assembly,
				 sizeof(input_assembly)) == NULL ||
	    CreateAssemblyObject(APP_EIP_OUTPUT_ASSEMBLY, output_assembly,
				 sizeof(output_assembly)) == NULL) {
		return kEipStatusError;
	}

	ConfigureExclusiveOwnerConnectionPoint(0, APP_EIP_OUTPUT_ASSEMBLY,
					 APP_EIP_INPUT_ASSEMBLY,
					 APP_EIP_CONFIG_ASSEMBLY);
	return kEipStatusOk;
}

void HandleApplication(void)
{
}

void CheckIoConnectionEvent(unsigned int output_assembly_id,
			    unsigned int input_assembly_id,
			    IoConnectionEvent io_connection_event)
{
	if (output_assembly_id != APP_EIP_OUTPUT_ASSEMBLY ||
	    input_assembly_id != APP_EIP_INPUT_ASSEMBLY) {
		return;
	}

	if (io_connection_event == kIoConnectionEventOpened) {
		atomic_inc(&io_connection_count);
		LOG_INF("EtherNet/IP I/O connection opened");
	} else {
		if (atomic_get(&io_connection_count) > 0) {
			atomic_dec(&io_connection_count);
		}
		LOG_INF("EtherNet/IP I/O connection closed: %d", io_connection_event);
	}
}

EipStatus AfterAssemblyDataReceived(CipInstance *instance)
{
	if (instance->instance_number != APP_EIP_OUTPUT_ASSEMBLY) {
		return kEipStatusOk;
	}

	for (uint16_t i = 0U; i < APP_EIP_ASSEMBLY_WORD_COUNT; i++) {
		int ret = app_modbus_scanner_holding_reg_wr(
			i, sys_get_le16(&output_assembly[i * sizeof(uint16_t)]));

		if (ret < 0) {
			LOG_WRN("Assembly %u write failed at word %u: %d",
				APP_EIP_OUTPUT_ASSEMBLY, i, ret);
			return kEipStatusError;
		}
	}
	return kEipStatusOk;
}

EipBool8 BeforeAssemblyDataSend(CipInstance *instance)
{
	if (instance->instance_number != APP_EIP_INPUT_ASSEMBLY) {
		return true;
	}

	for (uint16_t i = 0U; i < APP_EIP_ASSEMBLY_WORD_COUNT; i++) {
		uint16_t value = 0U;

		(void)app_modbus_scanner_holding_reg_rd(i, &value);
		sys_put_le16(value, &input_assembly[i * sizeof(uint16_t)]);
	}
	return true;
}

EipStatus ResetDevice(void)
{
	CloseAllConnections();
	CipQosUpdateUsedSetQosValues();
	(void)k_work_reschedule(&eip_reboot_work, K_MSEC(250));
	return kEipStatusOk;
}

EipStatus ResetDeviceToInitialConfiguration(void)
{
	/* Factory reset is deliberately not exposed until settings erasure is atomic. */
	return kEipStatusError;
}

void *CipCalloc(size_t number_of_elements, size_t size_of_element)
{
	size_t size;
	void *memory;

	if (size_of_element != 0U &&
	    number_of_elements > SIZE_MAX / size_of_element) {
		return NULL;
	}
	size = number_of_elements * size_of_element;
	memory = k_heap_alloc(&eip_heap, size, K_NO_WAIT);
	if (memory != NULL) {
		memset(memory, 0, size);
	}
	return memory;
}

void CipFree(void *data)
{
	k_heap_free(&eip_heap, data);
}

void RunIdleChanged(EipUint32 run_idle_value)
{
	if ((run_idle_value & 1U) != 0U) {
		CipIdentitySetExtendedDeviceStatus(kAtLeastOneIoConnectionInRunMode);
	} else {
		CipIdentitySetExtendedDeviceStatus(
			kAtLeastOneIoConnectionEstablishedAllInIdleMode);
	}
}
