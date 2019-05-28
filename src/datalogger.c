/******************************************************************************
*                                                                             *
*   Datalogger sample application for LS1043                                  *
*                                                                             *
*   (c) Copyright 2017 NXP. All rights reserved.                              *
*                                                                             *
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <time.h>
#include "sqlite3.h"
#include "jWrite.h"
#ifdef AWS_CLOUD
#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"
#endif
#ifdef AZURE_CLOUD
#include "iothub.h"
#include "iothub_device_client_ll.h"
#include "iothub_client_options.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "iothubtransportmqtt.h"

#ifdef SET_TRUSTED_CERT_IN_SAMPLES
#include "certs.h"
#endif // SET_TRUSTED_CERT_IN_SAMPLES
#endif

typedef struct canframe_s
{
   uint32_t timestamp : 16; //First 16 bits are for timestamp whose value will always be 0
   uint32_t MSGID : 12; // 12 bits of Message ID
   uint32_t DLC : 4; //4 bits defining the length of the CAN data
   char data[8]; //Data whose length would depend on the value specified by the DLC field
} canframe_t;

#define FAILURE -1


/************************************************************************************
 *     sqlite3 code
 ***********************************************************************************/

char sqlite3db[32] = "/sqlite3-db/calypso.db";

int32_t sqlite3_create_table()
{
    sqlite3 *db;
    char *err_msg = 0;

    int32_t rc = sqlite3_open(sqlite3db, &db);

    if (rc != SQLITE_OK) 
    {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        printf("\n ERROR: cannot open the database \n");
        sqlite3_close(db);
        return 1;
    }

    char *sql = "CREATE TABLE IF NOT EXISTS CalypsoData(deviceId text NOT NULL, messageId int, value long, dateTime date);";

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK ) 
    {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    }

    sqlite3_close(db);
    return 0;
}

int32_t sqlite3_insert_msg(char *deviceid,uint16_t msgid,uint64_t value,char *datetime)
{
    sqlite3 *db;
    char *err_msg = 0;
    char asql[1024]; 
    char *sql = asql;

    int32_t rc = sqlite3_open(sqlite3db, &db);

    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    sprintf(sql,"INSERT INTO CalypsoData VALUES('%s','%d',%lu, '%s');",deviceid,msgid,value,datetime);

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK ) 
    {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    }

    sqlite3_close(db);
    return 0;
}

int32_t callback(void *, int, char **, char **);


int32_t sqlite3_retrieve_msg(void) 
{

    sqlite3 *db;
    char *err_msg = 0;

    int32_t rc = sqlite3_open(sqlite3db, &db);

    if (rc != SQLITE_OK) {

        fprintf(stderr, "Cannot open database: %s\n",
                sqlite3_errmsg(db));
        sqlite3_close(db);

        return 1;
    }

    char *sql = "SELECT * FROM CalypsoData";

    rc = sqlite3_exec(db, sql, callback, 0, &err_msg);

    if (rc != SQLITE_OK ) 
    {
        fprintf(stderr, "Failed to select data\n");
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    }

    sqlite3_close(db);

    return 0;
}

int32_t callback(void *NotUsed, int32_t argc, char **argv, char **azColName) 
{
    NotUsed = 0;
    int32_t i;

    for (i = 0; i < argc; i++) 
    {
        printf("%s = %s | ", azColName[i], argv[i] ? argv[i] : "NULL");
    }

    printf("\n");
    return 0;
}


#ifdef AWS_CLOUD
/************************************************************************************
 *     aws iot c-sdk
 ***********************************************************************************/
char certDirectory[PATH_MAX + 1] = "aws_iot-c-sdk/certs";
char HostAddress[255] = AWS_IOT_MQTT_HOST;
uint32_t port = AWS_IOT_MQTT_PORT;
AWS_IoT_Client client;

void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) 
{
        printf(" MQTT Disconnect \n");
        IoT_Error_t rc = FAILURE;

        if(NULL == pClient) {
                return;
        }

        IOT_UNUSED(data);

        if(aws_iot_is_autoreconnect_enabled(pClient)) {
                printf(" Auto Reconnect is enabled, Reconnecting attempt will start now \n");
        } else {
                printf(" Auto Reconnect not enabled. Starting manual reconnect... \n");
                rc = aws_iot_mqtt_attempt_reconnect(pClient);
                if(NETWORK_RECONNECTED == rc) {
                        printf(" Manual Reconnect Successful \n");
                } else {
                        printf("Manual Reconnect Failed - %d \n", rc);
                }
        }
}

int32_t aws_iot_sdk_init()
{
        char rootCA[PATH_MAX + 1];
        char clientCRT[PATH_MAX + 1];
        char clientKey[PATH_MAX + 1];
        char CurrentWD[PATH_MAX + 1];

        IoT_Error_t rc = FAILURE;

        printf("\n AWS iOT init Start \n");
        IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
        IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

        printf(" AWS IoT SDK Version %d.%d.%d-%s\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

        getcwd(CurrentWD, sizeof(CurrentWD));
        snprintf(rootCA, PATH_MAX + 1, "%s/%s/%s", CurrentWD, certDirectory, AWS_IOT_ROOT_CA_FILENAME);
        snprintf(clientCRT, PATH_MAX + 1, "%s/%s/%s", CurrentWD, certDirectory, AWS_IOT_CERTIFICATE_FILENAME);
        snprintf(clientKey, PATH_MAX + 1, "%s/%s/%s", CurrentWD, certDirectory, AWS_IOT_PRIVATE_KEY_FILENAME);

        printf(" rootCA %s \n", rootCA);
        printf(" clientCRT %s \n", clientCRT);
        printf(" clientKey %s \n", clientKey);
        mqttInitParams.enableAutoReconnect = false; // We enable this later below
        mqttInitParams.pHostURL = HostAddress;
        mqttInitParams.port = port;
        mqttInitParams.pRootCALocation = rootCA;
        mqttInitParams.pDeviceCertLocation = clientCRT;
        mqttInitParams.pDevicePrivateKeyLocation = clientKey;
        mqttInitParams.mqttCommandTimeout_ms = 20000;
        mqttInitParams.tlsHandshakeTimeout_ms = 5000;
        mqttInitParams.isSSLHostnameVerify = true;
        mqttInitParams.disconnectHandler = disconnectCallbackHandler;
        mqttInitParams.disconnectHandlerData = NULL;

        rc = aws_iot_mqtt_init(&client, &mqttInitParams);
        if(SUCCESS != rc) 
        {
                printf(" aws_iot_mqtt_init returned error : %d \n", rc);
                return FAILURE;
        }

        connectParams.keepAliveIntervalInSec = 600;
        connectParams.isCleanSession = true;
        connectParams.MQTTVersion = MQTT_3_1_1;
        connectParams.pClientID = AWS_IOT_MQTT_CLIENT_ID;
        connectParams.clientIDLen = (uint16_t) strlen(AWS_IOT_MQTT_CLIENT_ID);
        connectParams.isWillMsgPresent = false;

        printf(" Connecting...");
        rc = aws_iot_mqtt_connect(&client, &connectParams);
        if(SUCCESS != rc) 
        {
                printf(" Error(%d) connecting to %s:%d \n", rc, mqttInitParams.pHostURL, mqttInitParams.port);
                return FAILURE;
        }
        /*
         * Enable Auto Reconnect functionality. Minimum and Maximum time of Exponential backoff are set in aws_iot_config.h
         *  #AWS_IOT_MQTT_MIN_RECONNECT_WAIT_INTERVAL
         *  #AWS_IOT_MQTT_MAX_RECONNECT_WAIT_INTERVAL
         */
        rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
        if(SUCCESS != rc) 
        {
                printf(" Unable to set Auto Reconnect to true - %d \n", rc);
                return FAILURE;
        }

        if(NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc)
        {
           printf("\n AWS iOT init is completed \n\n");
           return rc;
        }
        else
        {
           printf("\n AWS iOT init is FAILED \n");
           return FAILURE;
        }
}

int32_t iot_sdk_publish(char *buffer)
{
        IoT_Publish_Message_Params paramsQOS1;
        IoT_Error_t rc = FAILURE;

        paramsQOS1.qos = QOS1;
        paramsQOS1.payload = (void *)buffer;
        paramsQOS1.isRetained = 0;
        paramsQOS1.payloadLen = strlen(buffer);

        rc = aws_iot_mqtt_yield(&client, 100);
        while(NETWORK_ATTEMPTING_RECONNECT == rc) 
        {
           // If the client is attempting to reconnect we will skip the rest of the loop.
           rc = aws_iot_mqtt_yield(&client, 100);
           continue;
        }
        //rc = aws_iot_mqtt_publish(&client, "sdkTest/sub", 11, &paramsQOS1);
        rc = aws_iot_mqtt_publish(&client, "/sbs/devicedata/data", 20, &paramsQOS1);
        if (rc == MQTT_REQUEST_TIMEOUT_ERROR) 
        {
           return -1;
        }

        return 0;
}
#endif

#ifdef AZURE_CLOUD
/************************************************************************************
 *     azure iot c-sdk
 ***********************************************************************************/
/* Paste in the your iothub connection string  */
static const char* connectionString = "HostName=auto-iot.azure-devices.net;DeviceId=auto-device;SharedAccessKey=bVpdmEQs083yVjSNpHo0MgG0d2Q3kFH/f9n4+oRZbe0=";
#define MESSAGE_COUNT        5
static bool g_continueRunning = true;
static size_t g_message_count_send_confirmations = 0;
IOTHUB_DEVICE_CLIENT_LL_HANDLE device_ll_handle;

static void send_confirm_callback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    (void)userContextCallback;
    // When a message is sent this callback will get envoked
    g_message_count_send_confirmations++;
    //(void)printf("Confirmation callback received for message %zu with result %s\r\n", g_message_count_send_confirmations, ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result));
}

static void connection_status_callback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* user_context)
{
    (void)reason;
    (void)user_context;
    // This sample DOES NOT take into consideration network outages.
    if (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED)
    {
        (void)printf("The device client is connected to iothub\r\n");
    }
    else
    {
        (void)printf("The device client has been disconnected\r\n");
    }
}

int32_t azure_iot_sdk_init()
{
    IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol;

    // Select the Protocol to use with the connection
#ifdef SAMPLE_MQTT
    protocol = MQTT_Protocol;
#endif // SAMPLE_MQTT
#ifdef SAMPLE_AMQP
    protocol = AMQP_Protocol;
#endif // SAMPLE_AMQP

    // Used to initialize IoTHub SDK subsystem
    (void)IoTHub_Init();


    (void)printf("Creating IoTHub Device handle\r\n");
    // Create the iothub handle here
    device_ll_handle = IoTHubDeviceClient_LL_CreateFromConnectionString(connectionString, protocol);
    if (device_ll_handle == NULL)
    {
        (void)printf("Failure createing Iothub device.  Hint: Check you connection string.\r\n");
        return -1;
    }
    else
    {
        // Set any option that are neccessary.
        // For available options please see the iothub_sdk_options.md documentation

        bool traceOn = true;
        IoTHubDeviceClient_LL_SetOption(device_ll_handle, OPTION_LOG_TRACE, &traceOn);

#ifdef SET_TRUSTED_CERT_IN_SAMPLES
        // Setting the Trusted Certificate.  This is only necessary on system with without
        // built in certificate stores.
            IoTHubDeviceClient_LL_SetOption(device_ll_handle, OPTION_TRUSTED_CERT, certificates);
#endif // SET_TRUSTED_CERT_IN_SAMPLES

#if defined SAMPLE_MQTT || defined SAMPLE_MQTT_WS
        //Setting the auto URL Encoder (recommended for MQTT). Please use this option unless
        //you are URL Encoding inputs yourself.
        //ONLY valid for use with MQTT
        //bool urlEncodeOn = true;
        //IoTHubDeviceClient_LL_SetOption(iothub_ll_handle, OPTION_AUTO_URL_ENCODE_DECODE, &urlEncodeOn);
#endif

        // Setting connection status callback to get indication of connection to iothub
        (void)IoTHubDeviceClient_LL_SetConnectionStatusCallback(device_ll_handle, connection_status_callback, NULL);
        (void)printf("\n IoTHub Device handle created \n");
        return 0;
    }
}

int32_t iot_sdk_publish(char *buffer)
{
    IOTHUB_MESSAGE_HANDLE message_handle;

    // Construct the iothub message from a string or a byte array
    message_handle = IoTHubMessage_CreateFromString(buffer);
    //message_handle = IoTHubMessage_CreateFromByteArray((const unsigned char*)msgText, strlen(msgText)));

    // Set Message property
    /*(void)IoTHubMessage_SetMessageId(message_handle, "MSG_ID");
    (void)IoTHubMessage_SetCorrelationId(message_handle, "CORE_ID");
    (void)IoTHubMessage_SetContentTypeSystemProperty(message_handle, "application%2fjson");
    (void)IoTHubMessage_SetContentEncodingSystemProperty(message_handle, "utf-8");*/

    // Add custom properties to message
    (void)IoTHubMessage_SetProperty(message_handle, "property_key", "property_value");

    (void)printf("Sending message to IoTHub\r\n");
    IoTHubDeviceClient_LL_SendEventAsync(device_ll_handle, message_handle, send_confirm_callback, NULL);

    // The message is copied to the sdk so the we can destroy it
    IoTHubMessage_Destroy(message_handle);

    IoTHubDeviceClient_LL_DoWork(device_ll_handle);

    return 0;
}
#endif

/**************************************************************************
 *  calypso data processing
 *************************************************************************/

#define BUFLEN 512  //Max length of buffer
#define PORT 12345 //The port on which to listen for incoming data
int32_t sock;
char datetime[32];

int32_t get_date_time()
{
        time_t t = time(NULL);
        char default_format[] = "%Y-%m-%d %H:%M:%S";
        const char *format = default_format;

        struct tm lt;

        (void) localtime_r(&t, &lt);
        memset(datetime,0,32);
        if (strftime(datetime, sizeof(datetime), format, &lt) == 0) {
                (void) fprintf(stderr,  "strftime(3): cannot format supplied "
                                        "date/time into buffer of size %u "
                                        "using: '%s'\n",
                                        sizeof(datetime), format);
                return FAILURE;
        }

        //(void) printf("%u -> '%s'\n", (unsigned) t, datetime);
        return 0;
}

int32_t create_udp_socket_and_listen()
{
    struct sockaddr_in si_me;
     
    //create a UDP socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        printf("\n UDP socket creation failed \n");
        return -1;
    }
     
    // zero out the structure
    memset((char *) &si_me, 0, sizeof(si_me));
     
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(PORT);
    si_me.sin_addr.s_addr = inet_addr("192.168.0.11"); 
     
    //bind socket to port
    if( bind(sock , (struct sockaddr*)&si_me, sizeof(si_me) ) == -1)
    {
        printf("\n Binding UDP socket failed \n");
        return -1;
    }

    return 0; 
}

void prepare_message_and_log(uint16_t msgid, uint64_t value)
{
     char buffer[512];
     uint32_t buflen = 512;
     char *deviceid = "Auto_Gateway1";
     char str[32];

     memset(buffer,0,512);
     jwOpen( buffer, buflen, JW_OBJECT, JW_PRETTY );
#if 0
     switch(msgid)
     {
        case DRV_SEAT_MOVEMENT_ID:
           strcpy(str,"DRV SEAT MOVEMENT");
           jwObj_string( "deviceParameter", "DRV SEAT MOVEMENT" );
           jwObj_int( "deviceValue", value);
           break;
        case MOVE_ONLY_REAR_SUN_COVER_ID:
           strcpy(str,"MOVE ONLY REAR SUN COVER");
           jwObj_string( "deviceParameter", "MOVE ONLY REAR SUN COVER" );
           jwObj_int( "deviceValue", value);
           break;
        case ENGINE_ON_ID:
           strcpy(str,"ENGINE ON");
           jwObj_string( "deviceParameter", "ENGINE ON" );
           jwObj_int( "deviceValue", value);
           break;
        case ENGINE_OFF_ID:
           strcpy(str,"ENGINE OFF");
           jwObj_string( "deviceParameter", "ENGINE OFF" );
           jwObj_long( "deviceValue", value);
           break;
        case STEERING_WHEEL_TURN_ID:
           strcpy(str,"STEERING WHEEL TURN");
           jwObj_string( "deviceParameter", "STEERING WHEEL TURN" );
           jwObj_long( "deviceValue", value);
           break;
        case WHEEL_MOVEMENT_ID:
           if(value == 0xF2FF)
           {
              strcpy(str,"FRONT WHEEL MOVEMENT");
              jwObj_string( "deviceParameter", "FRONT WHEEL MOVEMENT" );
           }
           else if(value == 0xF1FF)
           {
              strcpy(str,"REAR WHEEL MOVEMENT");
              jwObj_string( "deviceParameter", "REAR WHEEL MOVEMENT" );
           }
           else
           {
              strcpy(str,"WHEEL MOVEMENT");
              jwObj_string( "deviceParameter", "WHEEL MOVEMENT" );
           }
           jwObj_int( "deviceValue", value);
           break;
        case GPS_ID:
           strcpy(str,"GPS");
           jwObj_string( "deviceParameter", "GPS" );
           jwObj_long( "deviceValue", value);
           break;
        case LIGHT_AND_BRAKE_ID:
           if(value == 0x0030FC)
           {
             strcpy(str,"LIGHT OFF NO BRAKE");
             jwObj_string( "deviceParameter", "LIGHT OFF NO BRAKE" );
           }
           else if(value == 0x2532FC)
           {
             strcpy(str,"FOG LAMP FRONT PRESS");
             jwObj_string( "deviceParameter", "FOG LAMP FRONT PRESS" );
           }
           else if(value == 0x0432FC)
           {
             strcpy(str,"PARKING LIGHT NO BRAKE");
             jwObj_string( "deviceParameter", "PARKING LIGHT NO BRAKE" );
           }
           else
           {
              strcpy(str,"LIGHT AND BRAKE");
              jwObj_string( "deviceParameter", "LIGHT AND BRAKE" );
           }
           jwObj_int( "deviceValue", value);
           break;
        case AIR_FLOW_FREE_FROM_OUTSIDE_ID:
           strcpy(str,"AIR FLOW FREE FROM OUTSIDE");
           jwObj_string( "deviceParameter", "AIR FLOW FREE FROM OUTSIDE" );
           jwObj_int( "deviceValue", value);
           break;
        case WINDOW_ID:
           if(value == 0x00F0)
           {
              strcpy(str,"WINDOW CLOSE");
              jwObj_string( "deviceParameter", "WINDOW CLOSE" );
           }
           else if(value == 0x5BF2)
           {
              strcpy(str,"WINDOW OPEN");
              jwObj_string( "deviceParameter", "WINDOW OPEN" );
           }
           else
           {
              strcpy(str,"WINDOW");
              jwObj_string( "deviceParameter", "WINDOW" );
           }
           jwObj_int( "deviceValue", value);
           break;
        case AC_ON_ID:
           strcpy(str,"AC ON");
           jwObj_string( "deviceParameter", "AC ON" );
           jwObj_int( "deviceValue", value);
           break;
        case HAZARD_WARNING_SIGNAL_ID:
           if(value == 0x80F0)
           {
              strcpy(str,"HAZARD WARNING SIGNAL OFF");
              jwObj_string( "deviceParameter", "HAZARD WARNING SIGNAL OFF" );
           }
           else if(value == 0xB1F2)
           {
              strcpy(str,"HAZARD WARNING SIGNAL PRESS");
              jwObj_string( "deviceParameter", "HAZARD WARNING SIGNAL PRESS" );
           } 
           else
           {
              strcpy(str,"HAZARD WARNING SIGNAL");
              jwObj_string( "deviceParameter", "HAZARD WARNING SIGNAL" );
           }
           jwObj_int( "deviceValue", value);
           break;
        case MENU_INTERACTION_HELP_ID:
           strcpy(str,"MENU INTERACTION HELP");
           jwObj_string( "deviceParameter", "MENU INTERACTION HELP" );
           jwObj_long( "deviceValue", value);
           break;
        case WIPERS_ID:
           if(value == 0xC1FF)
           {
              strcpy(str,"WIPERS ON WAY UP"); 
              jwObj_string( "deviceParameter", "WIPERS ON WAY UP" );
           }
           else if(value == 0xC2FF)
           {
              strcpy(str,"WIPERS ON WAY DOWN");
              jwObj_string( "deviceParameter", "WIPERS ON WAY DOWN" );
           }
           else
           {
              strcpy(str,"WIPERS ON WAY");
              jwObj_string( "deviceParameter", "WIPERS ON WAY" );
           }
           jwObj_int( "deviceValue", value);
           break;
        case BREAK_PRESS_ID:
           strcpy(str,"BREAK PRESS");
           jwObj_string( "deviceParameter", "BREAK PRESS" );
           jwObj_int( "deviceValue", value);
           break;
        default:
           printf("\n ERROR: not a valid MSGID \n");          
           break;
     }
#endif

    jwObj_string( "deviceId", deviceid);
    jwObj_int( "messageId", msgid);
    jwObj_long( "value", value);
    jwObj_string( "dateTime", datetime);
    jwClose();
    //sprintf(buffer,"%llu,%s,%s,%s",value,str,deviceid,datetime);
    //printf(" %s \n",buffer );         

    //if(1)
    if(iot_sdk_publish(buffer) == 0)
    {
       printf(" Device Id:%s , Message Id:%x , Value:%lx \n",deviceid,msgid,value);         
    }
    else
    {
       printf("\n ERROR: ioT publishing failed \n");         
    }
    sqlite3_insert_msg(deviceid,msgid,value,datetime);
    return;
}

uint64_t ntohll(uint64_t value)
{
    const uint32_t high_part = htonl((uint32_t)(value >> 32));
    const uint32_t low_part = htonl((uint32_t)(value & 0xFFFFFFFFLL));

    return ((uint64_t)(low_part) << 32) | high_part;
}

void process_buffer(char *buf, int32_t recv_len)
{
   uint16_t msgid = 0;
   uint16_t msgid_len;
   uint64_t value = 0;
   uint16_t msglen = 0;
   int32_t buflen = recv_len;
   int32_t ii;
 
#if 0
   printf("\n Buffer : ");
   for(ii = 0;ii < recv_len;ii++)
     printf("%x ",*(buf+ii));
   printf("\n");
#endif

   if(get_date_time() == FAILURE)
      return; 

   printf("\n");
   while(buflen > 0)
   {
     buf += 2;
     buflen -= 2;
     memcpy((char *)&msgid_len,buf,2);
     buf += 2;
     buflen -= 2;
     msgid_len = ntohs(msgid_len);
     msglen = msgid_len & 0x000F;
     msgid = (msgid_len >> 4) & 0x0FFF;
     if(msgid == 0)
     {
       //printf("\n END of messages \n");
       break;
     }
     memcpy((char *)&value,buf,msglen);
     buf += msglen;
     buflen -= msglen;
     value = ntohll(value);
     value = value >> ((8-msglen)*8);
     //printf("\n msgid:%x  msglen:%x  value: %lx \n",msgid,msglen,value);
     prepare_message_and_log(msgid,value);
     value = 0;
   }

   return;
}

int32_t main(int32_t argc, char *argv[])
{
        struct sockaddr si_other;
        int32_t slen = sizeof(si_other) , recv_len;
        char buf[BUFLEN];
         
#ifdef AWS_CLOUD
        if(aws_iot_sdk_init() == FAILURE)
        {
           return FAILURE; 
        }
#endif
#ifdef AZURE_CLOUD
        if(azure_iot_sdk_init() == FAILURE)
        {
           return FAILURE;
        }
#endif
        if(sqlite3_create_table() != 0)
        {
           return FAILURE; 
        }

        if(create_udp_socket_and_listen() != 0)
        {
           return FAILURE; 
        }

        while(1)
        {
           if ((recv_len = recvfrom(sock, buf, BUFLEN, 0, (struct sockaddr *)&si_other, &slen)) == -1)
           {
              printf("\n Receiving the data on UDP socket failed \n");
              return -1;
           }
         
           process_buffer(buf,recv_len);
        }

	printf("\nSuccessfully executed sample.\n");
	return 0;
}

