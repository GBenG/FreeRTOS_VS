/*
 * FreeRTOS V202212.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

/* Standard includes. */
#include <string.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* lwIP core includes */
#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/inet.h"
#include "lwip/dhcp.h"

/* applications includes */
#include "apps/httpserver_raw_from_lwIP_download/httpd.h"

/* include the port-dependent configuration */
#include "lwipcfg_msvc.h"

/* Dimensions the cTxBuffer array - which is itself used to hold replies from
command line commands.  cTxBuffer is a shared buffer, so protected by the
xTxBufferMutex mutex. */
#define lwipappsTX_BUFFER_SIZE	1024

/* The maximum time to block waiting to obtain the xTxBufferMutex to become
available. */
#define lwipappsMAX_TIME_TO_WAIT_FOR_TX_BUFFER_MS	( 100 / portTICK_RATE_MS )

/* Definitions of the various SSI callback functions within the pccSSITags
array.  If pccSSITags is updated, then these definitions must also be updated. */
#define ssiTASK_STATS_INDEX			0
#define ssiRUN_TIME_STATS_INDEX		1

/*-----------------------------------------------------------*/

/*
 * The function that implements the lwIP based sockets command interpreter
 * server.
 */
extern void vBasicSocketsCommandInterpreterTask( void *pvParameters );

/*
 * The SSI handler callback function passed to lwIP.
 */
static unsigned short uslwIPAppsSSIHandler( int iIndex, char *pcBuffer, int iBufferLength );

/*-----------------------------------------------------------*/

/* The SSI strings that are embedded in the served html files.  If this array
is changed, then the index position defined by the #defines such as
ssiTASK_STATS_INDEX above must also be updated. */
static const char *pccSSITags[] =
{
	"rtos_stats",
	"run_stats"
};

/* Semaphore used to guard the Tx buffer. */
static xSemaphoreHandle xTxBufferMutex = NULL;

/* The Tx buffer itself.  This is used to hold the text generated by the
execution of command line commands, and (hopefully) the execution of
server side include callbacks.  It is a shared buffer so protected by the
xTxBufferMutex mutex.  pcLwipAppsBlockingGetTxBuffer() and
vLwipAppsReleaseTxBuffer() are provided to obtain and release the
xTxBufferMutex respectively.  pcLwipAppsBlockingGetTxBuffer() must be used with
caution as it has the potential to block. */
static signed char cTxBuffer[ lwipappsTX_BUFFER_SIZE ];

/*-----------------------------------------------------------*/

void vStatusCallback( struct netif *pxNetIf )
{
char pcMessage[20];
struct in_addr* pxIPAddress;

	if( netif_is_up( pxNetIf ) != 0 )
	{
		strcpy( pcMessage, "IP=" );
		pxIPAddress = ( struct in_addr* ) &( pxNetIf->ip_addr );
		strcat( pcMessage, inet_ntoa( ( *pxIPAddress ) ) );
		xil_printf( pcMessage );
	}
	else
	{
		xil_printf( "Network is down" );
	}
}

/* Called from the TCP/IP thread. */
void lwIPAppsInit( void *pvArgument )
{
ip_addr_t xIPAddr, xNetMask, xGateway;
extern err_t xemacliteif_init(struct netif *netif);
extern void xemacif_input_thread( void *netif );
static struct netif xNetIf;

	( void ) pvArgument;

	/* Set up the network interface. */
	ip_addr_set_zero( &xGateway );
	ip_addr_set_zero( &xIPAddr );
	ip_addr_set_zero( &xNetMask );

	LWIP_PORT_INIT_GW(&xGateway);
	LWIP_PORT_INIT_IPADDR( &xIPAddr );
	LWIP_PORT_INIT_NETMASK(&xNetMask);

	/* Set mac address */
	xNetIf.hwaddr_len = 6;
	xNetIf.hwaddr[ 0 ] = configMAC_ADDR0;
	xNetIf.hwaddr[ 1 ] = configMAC_ADDR1;
	xNetIf.hwaddr[ 2 ] = configMAC_ADDR2;
	xNetIf.hwaddr[ 3 ] = configMAC_ADDR3;
	xNetIf.hwaddr[ 4 ] = configMAC_ADDR4;
	xNetIf.hwaddr[ 5 ] = configMAC_ADDR5;

	netif_set_default( netif_add( &xNetIf, &xIPAddr, &xNetMask, &xGateway, ( void * ) XPAR_AXI_ETHERNETLITE_0_BASEADDR, xemacliteif_init, tcpip_input ) );
	netif_set_status_callback( &xNetIf, vStatusCallback );
	#if LWIP_DHCP
	{
		dhcp_start( &xNetIf );
	}
	#else
	{
		netif_set_up( &xNetIf );
	}
	#endif

	/* Install the server side include handler. */
	http_set_ssi_handler( uslwIPAppsSSIHandler, pccSSITags, sizeof( pccSSITags ) / sizeof( char * ) );

	/* Create the mutex used to ensure mutual exclusive access to the Tx
	buffer. */
	xTxBufferMutex = xSemaphoreCreateMutex();
	configASSERT( xTxBufferMutex );

	/* Create the httpd server from the standard lwIP code.  This demonstrates
	use of the lwIP raw API. */
	httpd_init();

	sys_thread_new( "lwIP_In", xemacif_input_thread, &xNetIf, configMINIMAL_STACK_SIZE, configMAC_INPUT_TASK_PRIORITY );

	/* Create the FreeRTOS defined basic command server.  This demonstrates use
	of the lwIP sockets API. */
	xTaskCreate( vBasicSocketsCommandInterpreterTask, "CmdInt", configMINIMAL_STACK_SIZE * 5, NULL, configCLI_TASK_PRIORITY, NULL );
}
/*-----------------------------------------------------------*/

static unsigned short uslwIPAppsSSIHandler( int iIndex, char *pcBuffer, int iBufferLength )
{
static unsigned int uiUpdateCount = 0;
static char cUpdateString[ 200 ];
extern char *pcMainGetTaskStatusMessage( void );

	/* Unused parameter. */
	( void ) iBufferLength;

	/* The SSI handler function that generates text depending on the index of
	the SSI tag encountered. */

	switch( iIndex )
	{
		case ssiTASK_STATS_INDEX :
			vTaskList( pcBuffer );
			break;

		case ssiRUN_TIME_STATS_INDEX :
			vTaskGetRunTimeStats( pcBuffer );
			break;
	}

	/* Include a count of the number of times an SSI function has been executed
	in the returned string. */
	uiUpdateCount++;
	sprintf( cUpdateString, "\r\n\r\n%u\r\nStatus - %s", uiUpdateCount, pcMainGetTaskStatusMessage() );
	strcat( pcBuffer, cUpdateString );

	return strlen( pcBuffer );
}
/*-----------------------------------------------------------*/

signed char *pcLwipAppsBlockingGetTxBuffer( void )
{
signed char *pcReturn;

	/* Attempt to obtain the semaphore that guards the Tx buffer. */
	if( xSemaphoreTakeRecursive( xTxBufferMutex, lwipappsMAX_TIME_TO_WAIT_FOR_TX_BUFFER_MS ) == pdFAIL )
	{
		/* The semaphore could not be obtained before timing out. */
		pcReturn = NULL;
	}
	else
	{
		/* The semaphore was obtained successfully.  Return a pointer to the
		Tx buffer. */
		pcReturn = cTxBuffer;
	}

	return pcReturn;
}
/*-----------------------------------------------------------*/

void vLwipAppsReleaseTxBuffer( void )
{
	/* Finished with the Tx buffer.  Return the mutex. */
	xSemaphoreGiveRecursive( xTxBufferMutex );
}



