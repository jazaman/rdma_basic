#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <memory.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/0)."); } while (0)

static const int BUFFER_SIZE = 1024;
static const int EXITFAILURE = -1;
static const short DEFAULT_PORT = 9876;

struct context
{
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_channel;

    pthread_t cq_poller_thread;
};

struct connection
{
    struct ibv_qp *qp;

    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;

    char *recv_region;
    char *send_region;
};

static struct context *s_ctx = 0;

void die(const char* reason)
{
    fprintf(stderr, "%s\n", reason);
    exit(EXITFAILURE);
}

void on_completion(struct ibv_wc *wc)
{
    if (wc->status != IBV_WC_SUCCESS)
        die("on_completion: status is not IBV_WC_SUCCESS.");

    fprintf(stdout, "On Completion\n");

    if (wc->opcode & IBV_WC_RECV)
    {
        struct connection *conn = (struct connection *) (uintptr_t) wc->wr_id;

        printf("  -- received message: %s\n", conn->recv_region);

    }
    else if (wc->opcode == IBV_WC_SEND)
    {
        printf("  -- send completed successfully.\n");
    }
    else
    {
        printf("  -- Not sure what is completed, opcode: %d\n", wc->opcode);
    }
}

void * poll_cq(void *ctx)
{
    struct ibv_cq *cq;
    struct ibv_wc wc;

    while (1)
    {
        TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx));
        ibv_ack_cq_events(cq, 1);
        TEST_NZ(ibv_req_notify_cq(cq, 0));

        while (ibv_poll_cq(cq, 1, &wc))
            on_completion(&wc);
    }

    return 0;
}

void build_context(struct ibv_context *verbs)
{
    if (s_ctx)
    {
        if (s_ctx->ctx != verbs)
            die("cannot handle events in more than one context.");

        return;
    }

    s_ctx = (struct context *) malloc(sizeof(struct context));

    s_ctx->ctx = verbs;

    TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
    TEST_Z(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));
    TEST_Z(
            s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, 0, s_ctx->comp_channel,
                    0)); /* cqe=10 is arbitrary */
    TEST_NZ(ibv_req_notify_cq(s_ctx->cq, 0));

    TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, 0, poll_cq, 0));
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr)
{
    memset(qp_attr, 0, sizeof(*qp_attr));

    qp_attr->send_cq = s_ctx->cq;
    qp_attr->recv_cq = s_ctx->cq;
    qp_attr->qp_type = IBV_QPT_RC;

    qp_attr->cap.max_send_wr = 10;
    qp_attr->cap.max_recv_wr = 10;
    qp_attr->cap.max_send_sge = 1;
    qp_attr->cap.max_recv_sge = 1;
}

void post_receives(struct connection *conn)
{
    struct ibv_recv_wr wr, *bad_wr = 0;
    struct ibv_sge sge;

    wr.wr_id = (uintptr_t) conn;
    wr.next = 0;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    sge.addr = (uintptr_t) conn->recv_region;
    sge.length = BUFFER_SIZE;
    sge.lkey = conn->recv_mr->lkey;

    TEST_NZ(ibv_post_recv(conn->qp, &wr, &bad_wr));
}

void register_memory(struct connection *conn)
{
    conn->send_region = (char*) malloc(BUFFER_SIZE);
    conn->recv_region = (char*) malloc(BUFFER_SIZE);
    //should check for memory allocation failure BTW

    TEST_Z(
            conn->send_mr = ibv_reg_mr(s_ctx->pd, conn->send_region,
                    BUFFER_SIZE,
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));

    TEST_Z(
            conn->recv_mr = ibv_reg_mr(s_ctx->pd, conn->recv_region,
                    BUFFER_SIZE,
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));
}

int main(int argc, char* argv[])
{
    option long_options[] =
    {
    { "server", 0, 0, 's' },
    { "client", 1, 0, 'c' },
    { "port", 1, 0, 'p' },
    { 0, 0, 0, 0 } };

    int num_devices = 0;

    ibv_device** ib_devices = ibv_get_device_list(&num_devices);

    sockaddr_in addr;
    rdma_cm_event *event = 0;
    rdma_cm_id *listener = 0;
    rdma_event_channel *ec = 0;

    //Connection request
    ibv_qp_init_attr qp_attr;
    connection* conn;
    rdma_conn_param cm_params;

    //Connection
    ibv_send_wr wr, *bad_wr = 0;
    ibv_sge sge;

    char* address = 0;
    unsigned short port = DEFAULT_PORT;

    int opt = -1;

    bool done_option = false;

    while (!done_option)
    {
        opt = getopt_long(argc, argv, "s::c:p:", long_options, 0);
        printf("Option selected: %d", opt);
        switch (opt)
        {

        case -1:
            done_option = true;
            break;
        case 's':
            if (optarg)
            {
                fprintf(stdout, "Processing server option, Address: %s\n",
                        address);
                address = optarg;
                fprintf(stdout, "Address: %s\n", address);
            }
            break;
        case 'c':
            address = optarg;
            fprintf(stdout, "Processing client option, Address: %s\n", address);
            break;
        case 'p':
            TEST_Z(sscanf(optarg, "%hud", &port)); //unsigned short
            fprintf(stdout, "Processing port option: %d <- %s\n", port, optarg);
            break;
        default:
            fprintf(stderr, "Unrecognised option\n");
            fprintf(stderr,
                    "usage: server_rdma [-s<local address>] [-c <server address>] [-p port]\n");
            done_option = true;
            break;
        }
    }
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = address ? inet_addr(address) : INADDR_ANY;

    printf("%d device(s) are present on the system:\n", num_devices);

    if (ib_devices != 0)
    {
        int i = 0;
        for (; i < num_devices; ++i)
        {
            printf("%d. device = %s\n", i + 1,
                    ibv_get_device_name(ib_devices[i]));
        }
    }

    TEST_Z(ec = rdma_create_event_channel());
    TEST_NZ(rdma_create_id(ec, &listener, 0, RDMA_PS_TCP));
    TEST_NZ(rdma_bind_addr(listener, (struct sockaddr * )&addr));
    TEST_NZ(rdma_listen(listener, 10)); /* backlog=10 is arbitrary */

    port = ntohs(rdma_get_src_port(listener));
    printf("listening on port %d.\n", port);

    while (rdma_get_cm_event(ec, &event) == 0)
    {
        rdma_cm_event event_copy;
        //event only lasts till the next one is on the queue therefore,
        //copy it to a more sustainable location

        memcpy(&event_copy, event, sizeof(rdma_cm_event));
        rdma_ack_cm_event(event);
        switch (event_copy.event)
        {
        case RDMA_CM_EVENT_CONNECT_REQUEST:

            printf("Connection Requested\n");
            build_context(event_copy.id->verbs);
            printf("  -- Built context\n");
            build_qp_attr(&qp_attr);
            printf("  -- Built QP attributes\n");

            if (rdma_create_qp(event_copy.id, s_ctx->pd, &qp_attr) != 0)
            {
                printf("  -- ERROR: Failed to create Queue Pair\n");
            }

            event_copy.id->context = conn = (struct connection *) malloc(
                    sizeof(struct connection));
            conn->qp = event_copy.id->qp;

            register_memory(conn);
            post_receives(conn);

            memset(&cm_params, 0, sizeof(cm_params));
            TEST_NZ(rdma_accept(event_copy.id, &cm_params));

            break;
        case RDMA_CM_EVENT_ESTABLISHED:

            printf("Connection Established\n");
            printf("  -- connected\n");
            memset(&wr, 0, sizeof(wr));
            //memset(conn, 0 , sizeof(connection));
            event_copy.id->context = conn;
            snprintf(conn->send_region, BUFFER_SIZE,
                    "message from passive/server side with pid %d", getpid());
            wr.opcode = IBV_WR_SEND;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.send_flags = IBV_SEND_SIGNALED;
            sge.addr = (uintptr_t) conn->send_region;
            sge.length = BUFFER_SIZE;
            sge.lkey = conn->send_mr->lkey;
            TEST_NZ(ibv_post_send(conn->qp, &wr, &bad_wr));
            printf("  -- posting send ...\n");
            break;

        case RDMA_CM_EVENT_DISCONNECTED:
            printf("Connection disconnected\n");
            printf("peer disconnected.\n");

            rdma_destroy_qp(event_copy.id);

            ibv_dereg_mr(conn->send_mr);
            ibv_dereg_mr(conn->recv_mr);

            free(conn->send_region);
            free(conn->recv_region);

            free(conn);

            rdma_destroy_id(event_copy.id);
            break;

        default:
            break;

        }
    }
    return 0;
}
