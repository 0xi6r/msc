#include <stdio.h>
#include <string.h>

typedef enum RequestType {
    RequestOpen,
    RequestClose,
    RequestEcho,
    RequestUnknown
} RequestType;

typedef struct Request {
    RequestType type;
    const char *input;
    char *output;
    size_t output_size;
} Request;

typedef struct Driver {
    const char *name;
    int is_loaded;
} Driver;

static int DriverEntry(Driver *driver)
{
    driver->name = "SimpleEcho";
    driver->is_loaded = 1;
    printf("%s: loaded\n", driver->name);
    return 0;
}

static void DriverUnload(Driver *driver)
{
    printf("%s: unloaded\n", driver->name);
    driver->is_loaded = 0;
}

static int DispatchCreateClose(const Driver *driver, const Request *request)
{
    const char *action = request->type == RequestOpen ? "open" : "close";
    printf("%s: %s request completed\n", driver->name, action);
    return 0;
}

static int DispatchEcho(const Driver *driver, const Request *request)
{
    size_t needed;

    if (request->input == NULL || request->output == NULL) {
        return -1;
    }

    needed = strlen(request->input) + 1;
    if (request->output_size < needed) {
        return -2;
    }

    memcpy(request->output, request->input, needed);
    printf("%s: echoed %zu bytes\n", driver->name, needed - 1);
    return 0;
}

static int DispatchRequest(const Driver *driver, const Request *request)
{
    if (!driver->is_loaded) {
        return -10;
    }

    switch (request->type) {
    case RequestOpen:
    case RequestClose:
        return DispatchCreateClose(driver, request);
    case RequestEcho:
        return DispatchEcho(driver, request);
    default:
        return -20;
    }
}

int main(void)
{
    Driver driver = {0};
    char output[128] = {0};
    Request open_request = { RequestOpen, NULL, NULL, 0 };
    Request echo_request = { RequestEcho, "hello from a tiny driver model", output, sizeof output };
    Request close_request = { RequestClose, NULL, NULL, 0 };

    if (DriverEntry(&driver) != 0) {
        return 1;
    }

    if (DispatchRequest(&driver, &open_request) != 0) {
        return 1;
    }

    if (DispatchRequest(&driver, &echo_request) != 0) {
        return 1;
    }

    printf("user received: %s\n", output);

    if (DispatchRequest(&driver, &close_request) != 0) {
        return 1;
    }

    DriverUnload(&driver);
    return 0;
}
