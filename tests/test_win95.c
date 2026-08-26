/* Open Watcom Windows 95 platform adapter tests. */

#include "plat.h"
#include "zmodem_plat.h"

#include <stdio.h>
#include <stdlib.h>

static uint8_t mock_input[32];
static size_t mock_input_count;
static uint8_t mock_output[32];
static size_t mock_output_count;
static size_t mock_write_failure_after;
static COMMTIMEOUTS mock_original_timeouts;
static COMMTIMEOUTS mock_last_timeouts;
static unsigned mock_set_timeouts_calls;
static DWORD mock_purge_flags;

BOOL WINAPI
mock_GetCommTimeouts(HANDLE handle,LPCOMMTIMEOUTS timeouts)
{
	(void)handle;
	*timeouts = mock_original_timeouts;
	return TRUE;
}

BOOL WINAPI
mock_SetCommTimeouts(HANDLE handle,LPCOMMTIMEOUTS timeouts)
{
	(void)handle;
	mock_last_timeouts = *timeouts;
	mock_set_timeouts_calls++;
	return TRUE;
}

BOOL WINAPI
mock_ClearCommError(HANDLE handle,LPDWORD errors,LPCOMSTAT status)
{
	(void)handle;
	*errors = 0UL;
	(void)memset(status,0,sizeof(*status));
	status->cbInQue = (DWORD)mock_input_count;
	return TRUE;
}

BOOL WINAPI
mock_PurgeComm(HANDLE handle,DWORD flags)
{
	(void)handle;
	mock_purge_flags = flags;
	mock_input_count = 0U;
	return TRUE;
}

BOOL WINAPI
mock_ReadFile(HANDLE handle,LPVOID buffer,DWORD capacity,LPDWORD count,
    LPOVERLAPPED overlapped)
{
	size_t copied = mock_input_count;

	(void)handle;
	(void)overlapped;
	if (copied > (size_t)capacity) {
		copied = (size_t)capacity;
	}
	(void)memcpy(buffer,mock_input,copied);
	mock_input_count -= copied;
	(void)memmove(mock_input,&mock_input[copied],mock_input_count);
	*count = (DWORD)copied;
	return TRUE;
}

BOOL WINAPI
mock_WriteFile(HANDLE handle,LPCVOID buffer,DWORD length,LPDWORD count,
    LPOVERLAPPED overlapped)
{
	size_t copied = length > 2UL ? 2U : (size_t)length;

	(void)handle;
	(void)overlapped;
	if (mock_output_count >= mock_write_failure_after) {
		*count = 0UL;
		return FALSE;
	}
	if (copied > mock_write_failure_after - mock_output_count) {
		copied = mock_write_failure_after - mock_output_count;
	}
	(void)memcpy(&mock_output[mock_output_count],buffer,copied);
	mock_output_count += copied;
	*count = (DWORD)copied;
	return TRUE;
}

static bool
expect(bool condition,const char * description)
{
	if (!condition) {
		(void)fprintf(stderr,"test_win95: %s\n",description);
	}
	return condition;
}

static bool
test_options(void)
{
	struct zmodem_plat_io io;
	size_t index;
	bool passed = true;

	zmodem_plat_io_init(&io,0,1);
	index = 1U;
	passed = expect(zmodem_plat_parse_option(&io,ZMODEM_PLAT_ZMTX,
	    "-i",&index) == ZMODEM_PLAT_OPTION_ACCEPTED && io.escape_iac,
	    "parse IAC option") && passed;
	index = 1U;
	passed = expect(zmodem_plat_parse_option(&io,ZMODEM_PLAT_ZMTX,
	    "-c123",&index) == ZMODEM_PLAT_OPTION_ACCEPTED &&
	    io.transport == ZMODEM_WIN95_COM &&
	    (UINT_PTR)io.comm_handle == (UINT_PTR)123,
	    "parse COM handle") && passed;
	index = 1U;
	passed = expect(zmodem_plat_parse_option(&io,ZMODEM_PLAT_ZMTX,
	    "-t456",&index) == ZMODEM_PLAT_OPTION_INVALID,
	    "reject conflicting transports") && passed;

	zmodem_plat_io_init(&io,0,1);
	index = 1U;
	passed = expect(zmodem_plat_parse_option(&io,ZMODEM_PLAT_ZMRX,
	    "-t0",&index) == ZMODEM_PLAT_OPTION_ACCEPTED &&
	    io.transport == ZMODEM_WIN95_SOCKET && io.socket_handle == 0,
	    "parse zero socket handle") && passed;
	zmodem_plat_io_init(&io,0,1);
	index = 1U;
	passed = expect(zmodem_plat_parse_option(&io,ZMODEM_PLAT_ZMRX,
	    "-t4294967295",&index) == ZMODEM_PLAT_OPTION_INVALID,
	    "reject invalid socket handle") && passed;
	return passed;
}

static bool
test_com(void)
{
	static const uint8_t input[] = { 1U,2U,3U };
	static const uint8_t output[] = { 4U,5U,6U,7U,8U };
	struct zmodem_plat_io platform;
	struct zmodem_io io;
	uint8_t received[sizeof(input)];
	size_t count;
	bool passed = true;

	(void)memset(&mock_original_timeouts,0,sizeof(mock_original_timeouts));
	mock_original_timeouts.ReadIntervalTimeout = 17UL;
	mock_original_timeouts.WriteTotalTimeoutConstant = 29UL;
	mock_set_timeouts_calls = 0U;
	mock_output_count = 0U;
	mock_write_failure_after = (size_t)-1;
	mock_purge_flags = 0UL;
	zmodem_plat_io_init(&platform,0,1);
	platform.transport = ZMODEM_WIN95_COM;
	platform.comm_handle = (HANDLE)(UINT_PTR)123;
	passed = expect(zmodem_plat_io_make_raw(&platform) == 0,
	    "configure COM timeouts") && passed;
	passed = expect(mock_set_timeouts_calls == 1U &&
	    mock_last_timeouts.ReadIntervalTimeout == MAXDWORD &&
	    mock_last_timeouts.ReadTotalTimeoutMultiplier == 0UL &&
	    mock_last_timeouts.ReadTotalTimeoutConstant == 0UL &&
	    mock_last_timeouts.WriteTotalTimeoutConstant == 29UL,
	    "select immediate COM reads") && passed;

	zmodem_plat_io_bind(&io,&platform);
	(void)memcpy(mock_input,input,sizeof(input));
	mock_input_count = sizeof(input);
	passed = expect(io.poll(io.context) == 1,"poll queued COM data") && passed;
	passed = expect(io.read(io.context,received,sizeof(received),&count,0) ==
	    ZMODEM_OK && count == sizeof(received) &&
	    memcmp(received,input,sizeof(input)) == 0,
	    "read queued COM data") && passed;
	passed = expect(io.read(io.context,received,sizeof(received),&count,0) ==
	    ZMODEM_TIMEOUT,"time out empty COM read") && passed;
	passed = expect(io.write(io.context,output,sizeof(output)) == ZMODEM_OK &&
	    mock_output_count == 0U,"buffer COM output") && passed;
	passed = expect(io.flush(io.context) == ZMODEM_OK &&
	    mock_output_count == sizeof(output) &&
	    memcmp(mock_output,output,sizeof(output)) == 0,
	    "flush partial COM writes") && passed;
	mock_output_count = 0U;
	mock_write_failure_after = 2U;
	passed = expect(io.write(io.context,output,sizeof(output)) == ZMODEM_OK &&
	    io.flush(io.context) == ZMODEM_IO_ERROR &&
	    platform.output_count == sizeof(output) - 2U &&
	    memcmp(platform.output_buffer,&output[2],sizeof(output) - 2U) == 0,
	    "retain unsent COM output after failure") && passed;
	mock_write_failure_after = (size_t)-1;
	(void)memcpy(mock_input,input,sizeof(input));
	mock_input_count = sizeof(input);
	passed = expect(io.purge(io.context) == ZMODEM_OK &&
	    mock_input_count == 0U && mock_purge_flags == PURGE_RXCLEAR,
	    "purge COM input") && passed;
	passed = expect(zmodem_plat_io_close(&platform) == 0 &&
	    mock_set_timeouts_calls == 2U &&
	    mock_last_timeouts.ReadIntervalTimeout == 17UL,
	    "restore borrowed COM timeouts") && passed;
	return passed;
}

static bool
make_socket_pair(SOCKET * first,SOCKET * second)
{
	SOCKET listener = INVALID_SOCKET;
	struct sockaddr_in address;
	int address_length = sizeof(address);

	listener = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
	if (listener == INVALID_SOCKET) {
		return false;
	}
	(void)memset(&address,0,sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listener,(struct sockaddr *)&address,sizeof(address)) != 0 ||
	    listen(listener,1) != 0 ||
	    getsockname(listener,(struct sockaddr *)&address,&address_length) != 0) {
		(void)closesocket(listener);
		return false;
	}
	*first = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
	if (*first == INVALID_SOCKET ||
	    connect(*first,(struct sockaddr *)&address,sizeof(address)) != 0) {
		if (*first != INVALID_SOCKET) {
			(void)closesocket(*first);
		}
		(void)closesocket(listener);
		return false;
	}
	*second = accept(listener,NULL,NULL);
	(void)closesocket(listener);
	if (*second == INVALID_SOCKET) {
		(void)closesocket(*first);
		return false;
	}
	return true;
}

static bool
test_socket(void)
{
	static const uint8_t input[] = { 9U,10U,11U };
	static const uint8_t output[] = { 12U,13U,14U,15U };
	WSADATA data;
	SOCKET peer = INVALID_SOCKET;
	SOCKET adopted = INVALID_SOCKET;
	struct zmodem_plat_io platform;
	struct zmodem_io io;
	uint8_t received[8];
	size_t count;
	int result;
	bool passed = true;

	if (WSAStartup(MAKEWORD(1,1),&data) != 0 ||
	    !make_socket_pair(&peer,&adopted)) {
		(void)fprintf(stderr,"test_win95: create socket pair\n");
		return false;
	}
	zmodem_plat_io_init(&platform,0,1);
	platform.transport = ZMODEM_WIN95_SOCKET;
	platform.socket_handle = adopted;
	passed = expect(zmodem_plat_io_make_raw(&platform) == 0,
	    "initialize Winsock transport") && passed;
	zmodem_plat_io_bind(&io,&platform);
	passed = expect(io.poll(io.context) == 0,"poll empty socket") && passed;
	result = send(peer,(const char *)input,sizeof(input),0);
	passed = expect(result == sizeof(input) && io.poll(io.context) == 1,
	    "poll socket data") && passed;
	passed = expect(io.read(io.context,received,sizeof(received),&count,50) ==
	    ZMODEM_OK && count == sizeof(input) &&
	    memcmp(received,input,sizeof(input)) == 0,
	    "read socket data") && passed;
	passed = expect(io.read(io.context,received,sizeof(received),&count,5) ==
	    ZMODEM_TIMEOUT,"socket read timeout") && passed;
	passed = expect(io.write(io.context,output,sizeof(output)) == ZMODEM_OK &&
	    io.flush(io.context) == ZMODEM_OK,"write socket data") && passed;
	result = recv(peer,(char *)received,sizeof(received),0);
	passed = expect(result == sizeof(output) &&
	    memcmp(received,output,sizeof(output)) == 0,
	    "receive flushed socket data") && passed;
	result = send(peer,(const char *)input,sizeof(input),0);
	passed = expect(result == sizeof(input) &&
	    io.purge(io.context) == ZMODEM_OK && io.poll(io.context) == 0,
	    "purge socket data") && passed;
	passed = expect(zmodem_plat_io_close(&platform) == 0,
	    "close socket platform state") && passed;
	result = send(peer,(const char *)input,sizeof(input),0);
	passed = expect(result == sizeof(input),"leave borrowed socket open") && passed;
	(void)closesocket(peer);
	(void)closesocket(adopted);
	(void)WSACleanup();
	return passed;
}

static bool
write_payload(const char * path,uint8_t * expected,size_t length)
{
	FILE * stream;
	size_t index;
	size_t written;
	int close_result;

	for (index = 0U; index < length; index++) {
		expected[index] = (uint8_t)index;
	}
	stream = fopen(path,"wb");
	if (stream == NULL) {
		return false;
	}
	written = fwrite(expected,1U,length,stream);
	close_result = fclose(stream);
	return written == length && close_result == 0;
}

static bool
check_payload(const char * path,const uint8_t * expected,size_t length)
{
	uint8_t received[2048];
	FILE * stream = fopen(path,"rb");
	bool passed;

	if (stream == NULL) {
		return false;
	}
	passed = fread(received,1U,sizeof(received),stream) == length &&
	    memcmp(received,expected,length) == 0 && fgetc(stream) == EOF;
	(void)fclose(stream);
	return passed;
}

static bool
start_child(const char * program,char * command,const char * directory,
    PROCESS_INFORMATION * process)
{
	STARTUPINFOA startup;

	(void)memset(&startup,0,sizeof(startup));
	(void)memset(process,0,sizeof(*process));
	startup.cb = sizeof(startup);
	return CreateProcessA(program,command,NULL,NULL,TRUE,0UL,NULL,directory,
	    &startup,process) != FALSE;
}

static bool
test_program_transfer(void)
{
	static uint8_t expected[2048];
	WSADATA data;
	SOCKET sender_socket = INVALID_SOCKET;
	SOCKET receiver_socket = INVALID_SOCKET;
	char root[MAX_PATH];
	char sender_directory[MAX_PATH];
	char receiver_directory[MAX_PATH];
	char sender_program[MAX_PATH];
	char receiver_program[MAX_PATH];
	char sender_file[MAX_PATH];
	char receiver_file[MAX_PATH];
	char sender_command[MAX_PATH * 2];
	char receiver_command[MAX_PATH * 2];
	PROCESS_INFORMATION sender;
	PROCESS_INFORMATION receiver;
	DWORD sender_exit = STILL_ACTIVE;
	DWORD receiver_exit = STILL_ACTIVE;
	bool sender_started = false;
	bool receiver_started = false;
	bool winsock_started = false;
	bool passed = true;

	if (GetCurrentDirectoryA(sizeof(root),root) == 0UL) {
		return false;
	}
	(void)snprintf(sender_directory,sizeof(sender_directory),
	    "%s\\build\\win95\\test-send",root);
	(void)snprintf(receiver_directory,sizeof(receiver_directory),
	    "%s\\build\\win95\\test-receive",root);
	(void)snprintf(sender_program,sizeof(sender_program),
	    "%s\\build\\win95\\zmtx.exe",root);
	(void)snprintf(receiver_program,sizeof(receiver_program),
	    "%s\\build\\win95\\zmrx.exe",root);
	(void)snprintf(sender_file,sizeof(sender_file),"%s\\payload.bin",
	    sender_directory);
	(void)snprintf(receiver_file,sizeof(receiver_file),"%s\\payload.bin",
	    receiver_directory);
	if ((!CreateDirectoryA(sender_directory,NULL) &&
	    GetLastError() != ERROR_ALREADY_EXISTS) ||
	    (!CreateDirectoryA(receiver_directory,NULL) &&
	    GetLastError() != ERROR_ALREADY_EXISTS)) {
		return false;
	}
	(void)DeleteFileA(receiver_file);
	if (!write_payload(sender_file,expected,sizeof(expected)) ||
	    WSAStartup(MAKEWORD(1,1),&data) != 0) {
		passed = false;
		goto cleanup;
	}
	winsock_started = true;
	if (!make_socket_pair(&sender_socket,&receiver_socket)) {
		passed = false;
		goto cleanup;
	}
	(void)snprintf(receiver_command,sizeof(receiver_command),
	    "\"%s\" -t%lu -i",receiver_program,(unsigned long)receiver_socket);
	(void)snprintf(sender_command,sizeof(sender_command),
	    "\"%s\" -t%lu -i payload.bin",sender_program,
	    (unsigned long)sender_socket);
	receiver_started = start_child(receiver_program,receiver_command,
	    receiver_directory,&receiver);
	if (!receiver_started) {
		passed = false;
		goto cleanup;
	}
	sender_started = start_child(sender_program,sender_command,
	    sender_directory,&sender);
	if (!sender_started) {
		passed = false;
		goto cleanup;
	}
	if (WaitForSingleObject(sender.hProcess,30000UL) != WAIT_OBJECT_0 ||
	    WaitForSingleObject(receiver.hProcess,30000UL) != WAIT_OBJECT_0 ||
	    !GetExitCodeProcess(sender.hProcess,&sender_exit) ||
	    !GetExitCodeProcess(receiver.hProcess,&receiver_exit) ||
	    sender_exit != 0UL || receiver_exit != 0UL ||
	    !check_payload(receiver_file,expected,sizeof(expected))) {
		passed = false;
	}

cleanup:
	if (sender_started) {
		if (sender_exit == STILL_ACTIVE) {
			(void)TerminateProcess(sender.hProcess,1U);
			(void)WaitForSingleObject(sender.hProcess,5000UL);
		}
		(void)CloseHandle(sender.hThread);
		(void)CloseHandle(sender.hProcess);
	}
	if (receiver_started) {
		if (receiver_exit == STILL_ACTIVE) {
			(void)TerminateProcess(receiver.hProcess,1U);
			(void)WaitForSingleObject(receiver.hProcess,5000UL);
		}
		(void)CloseHandle(receiver.hThread);
		(void)CloseHandle(receiver.hProcess);
	}
	if (sender_socket != INVALID_SOCKET) {
		(void)closesocket(sender_socket);
	}
	if (receiver_socket != INVALID_SOCKET) {
		(void)closesocket(receiver_socket);
	}
	if (winsock_started) {
		(void)WSACleanup();
	}
	(void)DeleteFileA(sender_file);
	(void)DeleteFileA(receiver_file);
	(void)RemoveDirectoryA(sender_directory);
	(void)RemoveDirectoryA(receiver_directory);
	return expect(passed,"transfer between Win95 programs over inherited sockets");
}

int
main(void)
{
	struct zmodem_win95_timespec first;
	struct zmodem_win95_timespec second;
	bool passed = true;

	passed = test_options() && passed;
	passed = test_com() && passed;
	passed = test_socket() && passed;
	passed = test_program_transfer() && passed;
	passed = expect(zmodem_win95_clock_gettime(0,&first) == 0 &&
	    zmodem_win95_clock_gettime(0,&second) == 0 &&
	    (second.tv_sec > first.tv_sec ||
	    (second.tv_sec == first.tv_sec && second.tv_nsec >= first.tv_nsec)),
	    "monotonic clock") && passed;
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
