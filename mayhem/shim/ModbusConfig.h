/* Host-build configuration for the Mayhem harnesses: the library's ModbusConfigTemplate.h values. */
#ifndef MB_HOST_MODBUSCONFIG_H_
#define MB_HOST_MODBUSCONFIG_H_

#ifdef MB_SHIM_TCP
#define ENABLE_TCP 1
#else
#define ENABLE_TCP 0
#endif
#define ENABLE_USB_CDC 0
#define ENABLE_USART_DMA 0

#define T35  5
#define MAX_BUFFER  128
#define TIMEOUT_MODBUS 1000
#define MAX_M_HANDLERS 2
#define MAX_TELEGRAMS 2

#if ENABLE_TCP == 1
#define NUMBERTCPCONN   4
#define TCPAGINGCYCLES	1000
#endif

#endif
