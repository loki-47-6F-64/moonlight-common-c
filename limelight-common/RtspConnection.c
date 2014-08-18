#include "Limelight-internal.h"
#include "Rtsp.h"

static SOCKET sock = INVALID_SOCKET;
static IP_ADDRESS remoteAddr;
static int currentSeqNumber = 1;
static char* rtspTargetUrl;
static char sessionIdString[16];

// GFE 2.1.1
#define RTSP_CLIENT_VERSION 10
#define RTSP_CLIENT_VERSION_S "10"

#define RTSP_MAX_RESP_SIZE 1024

static POPTION_ITEM createOptionItem(char* option, char* content)
{
	POPTION_ITEM item = malloc(sizeof(*item));
	if (item == NULL) {
		return NULL;
	}

	item->option = malloc(strlen(option) + 1);
	if (item->option == NULL) {
		free(item);
		return NULL;
	}

	strcpy(item->option, option);

	item->content = malloc(strlen(content) + 1);
	if (item->content == NULL) {
		free(item->option);
		free(item);
		return NULL;
	}

	strcpy(item->content, content);

	item->next = NULL;
	item->flags = FLAG_ALLOCATED_OPTION_FIELDS;

	return item;
}

static int addOption(PRTSP_MESSAGE msg, char* option, char* content)
{
	POPTION_ITEM item = createOptionItem(option, content);
	if (item == NULL) {
		return 0;
	}

	insertOption(&msg->options, item);
	msg->flags |= FLAG_ALLOCATED_OPTION_ITEMS;

	return 1;
}

static int initializeRtspRequest(PRTSP_MESSAGE msg, char* command, char* target)
{
	createRtspRequest(msg, NULL, 0, command, target, "RTSP/1.0",
		currentSeqNumber++, NULL, NULL);
	
	if (!addOption(msg, "X-GS-ClientVersion", RTSP_CLIENT_VERSION_S)) {
		freeMessage(msg);
		return 0;
	}

	return 1;
}

/* Returns 1 on success, 0 otherwise */
static int transactRtspMessage(PRTSP_MESSAGE request, PRTSP_MESSAGE response) {
	int err, ret = 0;
	char responseBuffer[RTSP_MAX_RESP_SIZE];
	int offset;
	PRTSP_MESSAGE responseMsg = NULL;
	char* serializedMessage = NULL;
	int messageLen;

	sock = connectTcpSocket(remoteAddr, 48010);
	if (sock == INVALID_SOCKET) {
		return ret;
	}
	enableNoDelay(sock);

	serializedMessage = serializeRtspMessage(request, &messageLen);
	if (serializedMessage == NULL) {
		closesocket(sock);
		return ret;
	}

	// Send our message
	err = send(sock, serializedMessage, messageLen, 0);
	if (err == SOCKET_ERROR) {
		goto Exit;
	}

	// Read the response until the server closes the connection
	offset = 0;
	for (;;) {
		err = recv(sock, &responseBuffer[offset], RTSP_MAX_RESP_SIZE - offset, 0);
		if (err <= 0) {
			// Done reading
			break;
		}
		offset += err;

		// Warn if the RTSP message is too big
		if (offset == RTSP_MAX_RESP_SIZE) {
			Limelog("RTSP message too long\n");
			goto Exit;
		}
	}

	if (parseRtspMessage(response, responseBuffer) == RTSP_ERROR_SUCCESS) {
		// Successfully parsed response
		ret = 1;
	}
	else {
		Limelog("Failed to parse RTSP response\n");
	}

Exit:
	if (serializedMessage != NULL) {
		free(serializedMessage);
	}

	closesocket(sock);
	sock = INVALID_SOCKET;
	return ret;
}

void terminateRtspHandshake(void) {
	if (sock != INVALID_SOCKET) {
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
}

static int requestOptions(PRTSP_MESSAGE response) {
	RTSP_MESSAGE request;
	int ret;

	ret = initializeRtspRequest(&request, "OPTIONS", rtspTargetUrl);
	if (ret != 0) {
		ret = transactRtspMessage(&request, response);
		freeMessage(&request);
	}

	return ret;
}

static int requestDescribe(PRTSP_MESSAGE response) {
	RTSP_MESSAGE request;
	int ret;

	ret = initializeRtspRequest(&request, "DESCRIBE", rtspTargetUrl);
	if (ret != 0) {
		if (addOption(&request, "Accept",
				"application/sdp") &&
			addOption(&request, "If-Modified-Since",
				"Thu, 01 Jan 1970 00:00:00 GMT")) {
			ret = transactRtspMessage(&request, response);
		}
		else {
			ret = 0;
		}
		freeMessage(&request);
	}

	return ret;
}

static int setupStream(PRTSP_MESSAGE response, char* target) {
	RTSP_MESSAGE request;
	int ret;

	ret = initializeRtspRequest(&request, "SETUP", target);
	if (ret != 0) {
		if (sessionIdString[0] != 0) {
			if (!addOption(&request, "Session", sessionIdString)) {
				ret = 0;
				goto FreeMessage;
			}
		}

		if (addOption(&request, "Transport", " ") &&
			addOption(&request, "If-Modified-Since",
				"Thu, 01 Jan 1970 00:00:00 GMT")) {
			ret = transactRtspMessage(&request, response);
		}
		else {
			ret = 0;
		}

	FreeMessage:
		freeMessage(&request);
	}

	return ret;
}

static int playStream(PRTSP_MESSAGE response, char* target) {
	RTSP_MESSAGE request;
	int ret;

	ret = initializeRtspRequest(&request, "PLAY", target);
	if (ret != 0) {
		if (addOption(&request, "Session", sessionIdString)) {
			ret = transactRtspMessage(&request, response);
		}
		else {
			ret = 0;
		}
		freeMessage(&request);
	}

	return ret;
}

static int sendVideoAnnounce(PRTSP_MESSAGE response, PSTREAM_CONFIGURATION streamConfig) {
	RTSP_MESSAGE request;
	int ret;
	int payloadLength;
	char payloadLengthStr[16];
	struct in_addr sdpAddr;

	ret = initializeRtspRequest(&request, "ANNOUNCE", "streamid=video");
	if (ret != 0) {
		ret = 0;

		if (!addOption(&request, "Session", sessionIdString) ||
			!addOption(&request, "Content-type", "application/sdp")) {
			goto FreeMessage;
		}

		sdpAddr.S_un.S_addr = remoteAddr;
		request.payload = getSdpPayloadForStreamConfig(streamConfig, sdpAddr, &payloadLength);
		if (request.payload == NULL) {
			goto FreeMessage;
		}
		request.flags |= FLAG_ALLOCATED_PAYLOAD;

		sprintf(payloadLengthStr, "%d", payloadLength);
		if (!addOption(&request, "Content-length", payloadLengthStr)) {
			goto FreeMessage;
		}

		ret = transactRtspMessage(&request, response);

	FreeMessage:
		freeMessage(&request);
	}

	return ret;
}

int performRtspHandshake(IP_ADDRESS addr, PSTREAM_CONFIGURATION streamConfigPtr) {
	remoteAddr = addr;

	{
		RTSP_MESSAGE response;

		if (!requestOptions(&response)) {
			Limelog("RTSP OPTIONS request failed\n");
			return -1;
		}

		if (response.message.response.statusCode != 200) {
			Limelog("RTSP OPTIONS request failed: %d\n",
				response.message.response.statusCode);
			return -1;
		}

		freeMessage(&response);
	}

	{
		RTSP_MESSAGE response;

		if (!requestDescribe(&response)) {
			Limelog("RTSP DESCRIBE request failed\n");
			return -1;
		}

		if (response.message.response.statusCode != 200) {
			Limelog("RTSP DESCRIBE request failed: %d\n",
				response.message.response.statusCode);
			return -1;
		}

		freeMessage(&response);
	}

	{
		RTSP_MESSAGE response;
		char* sessionId;

		if (!setupStream(&response, "streamid=audio")) {
			Limelog("RTSP SETUP streamid=audio request failed\n");
			return -1;
		}

		if (response.message.response.statusCode != 200) {
			Limelog("RTSP SETUP streamid=audio request failed: %d\n",
				response.message.response.statusCode);
			return -1;
		}

		sessionId = getOptionContent(response.options, "Session");
		if (sessionId == NULL) {
			Limelog("RTSP SETUP streamid=audio is missing session attribute");
			return -1;
		}

		freeMessage(&response);
	}

	{
		RTSP_MESSAGE response;

		if (!setupStream(&response, "streamid=video")) {
			Limelog("RTSP SETUP streamid=video request failed\n");
			return -1;
		}

		if (response.message.response.statusCode != 200) {
			Limelog("RTSP SETUP streamid=video request failed: %d\n",
				response.message.response.statusCode);
			return -1;
		}

		freeMessage(&response);
	}

	{
		RTSP_MESSAGE response;

		if (!sendVideoAnnounce(&response, streamConfigPtr)) {
			Limelog("RTSP ANNOUNCE request failed\n");
			return -1;
		}

		if (response.message.response.statusCode != 200) {
			Limelog("RTSP ANNOUNCE request failed: %d\n",
				response.message.response.statusCode);
			return -1;
		}

		freeMessage(&response);
	}

	{
		RTSP_MESSAGE response;

		if (!playStream(&response, "streamid=video")) {
			Limelog("RTSP PLAY streamid=video request failed\n");
			return -1;
		}

		if (response.message.response.statusCode != 200) {
			Limelog("RTSP PLAY streamid=video failed: %d\n",
				response.message.response.statusCode);
			return -1;
		}

		freeMessage(&response);
	}

	{
		RTSP_MESSAGE response;

		if (!playStream(&response, "streamid=audio")) {
			Limelog("RTSP PLAY streamid=audio request failed\n");
			return -1;
		}

		if (response.message.response.statusCode != 200) {
			Limelog("RTSP PLAY streamid=audio failed: %d\n",
				response.message.response.statusCode);
			return -1;
		}

		freeMessage(&response);
	}

	return 0;
}