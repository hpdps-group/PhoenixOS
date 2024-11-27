#include <iostream>
#include <fstream>

#include <signal.h>
#include <stdint.h>
#include <string.h>

#include "pos/common.h"
#include "pos/transport.h"
#include "pos/cuda_impl/workspace.h"

#include "cpu_rpc_prot.h"

POSWorkspace_CUDA<POSTransport_SHM> *pos_cuda_ws;
bool mock_stop = false;
uint64_t client_uuid;
uint64_t module_key = 0x200000000000;

void int_handler(int signal) {
    mock_stop = true;
}

void test_cuModuleLoadData(){
    /*!
     *  \brief  fatbin header definition
     */
    typedef struct __attribute__((__packed__)) fat_header {
        uint32_t magic;
        uint32_t version;
        uint64_t text;      // points to first text section
        uint64_t data;      // points to outside of the file
        uint64_t unknown;
        uint64_t text2;     // points to second text section
        uint64_t zero;
    } fat_header_t;

    /*!
     *  \brief  fatbin ELF header definition
     */
    typedef struct  __attribute__((__packed__)) fat_elf_header{
        uint32_t magic;
        uint16_t version;
        uint16_t header_size;
        uint64_t size;
    } fat_elf_header_t;

    std::ifstream file("./pos-test.fatbin", std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    CUresult res;
    fat_header_t *fat_hdr;
    fat_elf_header_t *elf_hdr;

    if (file.read(buffer.data(), size)){
        POS_CHECK_POINTER(elf_hdr = (fat_elf_header_t*)buffer.data());
        // POS_CHECK_POINTER(elf_hdr = fat_hdr->text);

        POS_DEBUG(
            "readin file: magic(%#x), vector size(%lu), size(%lu)", 
            elf_hdr->magic, buffer.size(), elf_hdr->header_size + elf_hdr->size
        );

        res = pos_cuda_ws->pos_process( 
            /* api_id */ rpc_cuModuleLoad, 
            /* uuid */ client_uuid, 
            /* param_desps */ {
                { .value = &module_key, .size = sizeof(uint64_t) },
                { .value = elf_hdr, .size = elf_hdr->header_size + elf_hdr->size }
            },
            /* ret_data */ nullptr
        );
        POS_DEBUG("(test_cuModuleLoadData): pos_process return %d", res);
    } else {
        POS_ERROR("(test_cuModuleLoadData): failed to open the fatbin file");
    }
}

void test_cuModuleGetFunction(){
    CUresult res;
    uint64_t mock_host_func = 0x300000000000;
    int mock_thread_limit = 16;

    char str_1[] = "nothing";
    char str_2[] = "_Z8kernel_1PKfPfS1_S1_i";

    res = pos_cuda_ws->pos_process( 
        /* api_id */ rpc_cuModuleGetFunction, 
        /* uuid */ client_uuid, 
        /* param_desps */ {
            { .value = &module_key, .size = sizeof(uint64_t) },
            { .value = &mock_host_func, .size = sizeof(uint64_t) },
            { .value = str_1, .size = strlen(str_1)+1 },
            { .value = str_2, .size = strlen(str_2)+1 },
            { .value = &mock_thread_limit, .size = sizeof(int) },
        },
        /* ret_data */ nullptr
    );
    POS_DEBUG("(test_cuModuleGetFunction): pos_process return %d", res);
}

int main(){
    struct sigaction act;
    POSClient_CUDA clnt;
    clnt.init();

    act.sa_handler = int_handler;
    sigaction(SIGINT, &act, NULL);
    
    pos_cuda_ws = new POSWorkspace_CUDA<POSTransport_SHM>();
    POS_CHECK_POINTER(pos_cuda_ws);
    pos_cuda_ws->init();

    pos_cuda_ws->create_client(&clnt, &client_uuid);
    pos_cuda_ws->create_qp(client_uuid);

    test_cuModuleLoadData();
    test_cuModuleGetFunction();

    while(!mock_stop){}

    delete pos_cuda_ws;
}