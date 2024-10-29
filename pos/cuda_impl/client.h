/*
 * Copyright 2024 The PhoenixOS Authors. All rights reserved.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <iostream>
#include <set>
#include <filesystem>

#include "pos/include/common.h"
#include "pos/include/workspace.h"
#include "pos/include/client.h"
#include "pos/include/transport.h"

#include "pos/cuda_impl/handle.h"
#include "pos/cuda_impl/handle/cublas.h"
#include "pos/cuda_impl/api_index.h"
#include "pos/cuda_impl/parser.h"
#include "pos/cuda_impl/worker.h"


/*!
 *  \brief  context of CUDA client
 */
typedef struct pos_client_cxt_CUDA {
    POS_CLIENT_CXT_HEAD;
} pos_client_cxt_CUDA_t;


class POSClient_CUDA : public POSClient {
 public:
    /*!
     *  \brief  constructor
     *  \param  id  client identifier
     *  \param  cxt context to initialize this client
     */
    POSClient_CUDA(POSWorkspace *ws, uint64_t id, pos_client_cxt_CUDA_t cxt) 
        : POSClient(id, cxt.cxt_base, ws), _cxt_CUDA(cxt)
    {
        // raise parser thread
        this->parser = new POSParser_CUDA(ws, this);
        POS_CHECK_POINTER(this->parser);
        this->parser->init();

        // raise worker thread
        this->worker = new POSWorker_CUDA(ws, this);
        POS_CHECK_POINTER(this->worker);
        this->worker->init();
  
        if(unlikely(POS_SUCCESS != this->init_transport())){
          POS_WARN_C("failed to initialize transport for client %lu, migration would be failed", id);
        }
    }
    POSClient_CUDA(){}
    
    
    /*!
     *  \brief  deconstructor
     */
    ~POSClient_CUDA(){
        // shutdown parser and worker
        if(this->parser != nullptr){ delete this->parser; }
        if(this->worker != nullptr){ delete this->worker; }
    }
    

    /*!
     *  \brief  instantiate handle manager for all used resources
     *  \note   the children class should replace this method to initialize their 
     *          own needed handle managers
     *  \return POS_SUCCESS for successfully initialization
     */
    pos_retval_t init_handle_managers() override {
        pos_retval_t retval = POS_SUCCESS;
        std::vector<POSHandle_CUDA_Device*> device_handles;
        std::vector<POSHandle_CUDA_Context*> context_handles;

        POSHandleManager_CUDA_Device *device_mgr;
        POSHandleManager_CUDA_Context *ctx_mgr;
        POSHandleManager_CUDA_Stream *stream_mgr;
        POSHandleManager_cuBLAS_Context *cublas_context_mgr;
        POSHandleManager_CUDA_Event *event_mgr;
        POSHandleManager_CUDA_Module *module_mgr;
        POSHandleManager_CUDA_Function *function_mgr;
        POSHandleManager_CUDA_Var *var_mgr;
        POSHandleManager_CUDA_Memory *memory_mgr;

        std::map<uint64_t, std::vector<POSHandle*>> related_handles;
        bool is_restoring = this->_cxt.checkpoint_file_path.size() > 0;


        auto __cast_to_base_handle_list = [](auto handle_list) -> std::vector<POSHandle*> {
            std::vector<POSHandle*> ret_list;
            for(auto handle : handle_list){ ret_list.push_back(handle); }
            return ret_list;
        };


        /*!
         *  \note   Hierarchy of CUDA Resources
             ╔══════════════════════════════════════════════════════════════════════╗
            ╔══════════════════════════════════════════════════════════════════════╗║
            ║                              CUDA Device                             ║║
            ╠══════════════════════════════════════════════════════════════════════╣║
            ║                             CUDA Context                             ║║
            ╠════════════════╦════════════╦══════════════════════════╦═════════════╣║
            ║   CUDA Stream  ║            ║        CUDA Module       ║             ║║
            ╠════════════════╣ CUDA Event ╠═══════════════╦══════════╣ CUDA Memory ║║
            ║ cuBLAS Context ║            ║ CUDA Function ║ CUDA Var ║             ║╝
            ╚════════════════╩════════════╩═══════════════╩══════════╩═════════════╝
         */


        // CUDA device handle manager
        related_handles.clear();
        POS_CHECK_POINTER(device_mgr = new POSHandleManager_CUDA_Device());
        if(unlikely(POS_SUCCESS != (
            retval = device_mgr->init(related_handles)
        ))){
            POS_WARN_C("failed to initialize CUDA device handle manager, client won't be run");
            goto exit;
        }
        this->handle_managers[kPOS_ResourceTypeId_CUDA_Device] = device_mgr;

        // CUDA context handle manager
        related_handles.clear();
        device_handles = device_mgr->get_handles();
        POS_ASSERT(device_handles.size() > 0);
        related_handles.insert({ kPOS_ResourceTypeId_CUDA_Device, __cast_to_base_handle_list(device_handles) });
        POS_CHECK_POINTER(ctx_mgr = new POSHandleManager_CUDA_Context());
        if(unlikely(POS_SUCCESS != (
            retval = ctx_mgr->init(related_handles)
        ))){
            POS_WARN_C("failed to initialize CUDA context handle manager, client won't be run");
            goto exit;
        }
        this->handle_managers[kPOS_ResourceTypeId_CUDA_Context] = ctx_mgr;

        // CUDA stream handle manager
        related_handles.clear();
        context_handles = ctx_mgr->get_handles();
        POS_ASSERT(context_handles.size() > 0);
        related_handles.insert({ kPOS_ResourceTypeId_CUDA_Context, __cast_to_base_handle_list(context_handles) });
        POS_CHECK_POINTER(stream_mgr = new POSHandleManager_CUDA_Stream());
        if(unlikely(POS_SUCCESS != (
            retval = stream_mgr->init(related_handles)
        ))){
            POS_WARN_C("failed to initialize CUDA stream handle manager, client won't be run");
            goto exit;
        }
        this->handle_managers[kPOS_ResourceTypeId_CUDA_Stream] = stream_mgr;
        
        // cuBLAS context handle manager
        related_handles.clear();
        POS_CHECK_POINTER(cublas_context_mgr = new POSHandleManager_cuBLAS_Context());
        if(unlikely(POS_SUCCESS != (
            retval = cublas_context_mgr->init(related_handles)
        ))){
            POS_WARN_C("failed to initialize cuBLAS context handle manager, client won't be run");
            goto exit;
        }
        this->handle_managers[kPOS_ResourceTypeId_cuBLAS_Context] = cublas_context_mgr;

        // CUDA event handle manager
        related_handles.clear();
        POS_CHECK_POINTER(event_mgr = new POSHandleManager_CUDA_Event());
        if(unlikely(POS_SUCCESS != (
            retval = event_mgr->init(related_handles)
        ))){
            POS_WARN_C("failed to initialize CUDA event handle manager, client won't be run");
            goto exit;
        }
        this->handle_managers[kPOS_ResourceTypeId_CUDA_Event] = event_mgr;

        // CUDA module handle manager
        related_handles.clear();
        POS_CHECK_POINTER(module_mgr = new POSHandleManager_CUDA_Module());
        if(unlikely(POS_SUCCESS != (
            retval = module_mgr->init(related_handles)
        ))){
            POS_WARN_C("failed to initialize CUDA module handle manager, client won't be run");
            goto exit;
        }
        this->handle_managers[kPOS_ResourceTypeId_CUDA_Module] = module_mgr;
        if(std::filesystem::exists(this->_cxt.kernel_meta_path)){
            POS_DEBUG_C("loading kernel meta from cache %s...", this->_cxt.kernel_meta_path.c_str());
            retval = module_mgr->load_cached_function_metas(this->_cxt.kernel_meta_path);
            if(likely(retval == POS_SUCCESS)){
                this->_cxt.is_load_kernel_from_cache = true;
                POS_BACK_LINE
                POS_DEBUG_C("loading kernel meta from cache %s [done]", this->_cxt.kernel_meta_path.c_str());
            } else {
                POS_WARN_C("loading kernel meta from cache %s [failed]", this->_cxt.kernel_meta_path.c_str());
            }
        }

        // CUDA function handle manager
        related_handles.clear();
        POS_CHECK_POINTER(function_mgr = new POSHandleManager_CUDA_Function());
        if(unlikely(POS_SUCCESS != (
            retval = function_mgr->init(related_handles)
        ))){
            POS_WARN_C("failed to initialize CUDA function handle manager, client won't be run");
            goto exit;
        }
        this->handle_managers[kPOS_ResourceTypeId_CUDA_Function] = function_mgr;

        // CUDA var handle manager
        related_handles.clear();
        POS_CHECK_POINTER(var_mgr = new POSHandleManager_CUDA_Var());
        if(unlikely(POS_SUCCESS != (
            retval = var_mgr->init(related_handles)
        ))){
            POS_WARN_C("failed to initialize CUDA var handle manager, client won't be run");
            goto exit;
        }
        this->handle_managers[kPOS_ResourceTypeId_CUDA_Var] = var_mgr;

        // CUDA memory handle manager
        related_handles.clear();
        context_handles = ctx_mgr->get_handles();
        POS_ASSERT(context_handles.size() > 0);
        related_handles.insert({ kPOS_ResourceTypeId_CUDA_Context, __cast_to_base_handle_list(context_handles) });
        POS_CHECK_POINTER(memory_mgr = new POSHandleManager_CUDA_Memory());
        if(unlikely(POS_SUCCESS != (
            retval = memory_mgr->init(related_handles)
        ))){
            POS_WARN_C("failed to initialize CUDA memory handle manager, client won't be run");
            goto exit;
        }
        this->handle_managers[kPOS_ResourceTypeId_CUDA_Memory] = memory_mgr;

    exit:
        return retval;
    }


    /*
     *  \brief  initialization of transport utilities for migration  
     *  \return POS_SUCCESS for successfully initialization
     */
    pos_retval_t init_transport() override {
      pos_retval_t retval = POS_SUCCESS;
      
      // TODO: default to use RDMA here, might support other transport later
      this->_transport = new POSTransport_RDMA</* is_server */false>(/* dev_name */ "");
      POS_CHECK_POINTER(this->_transport);

    exit:
        return retval;
    }


    /*!
     *  \brief      deinit handle manager for all used resources
     *  \example    CUDA function manager should export the metadata of functions
     */
    void deinit_dump_handle_managers() override {
        this->__dump_hm_cuda_functions();
    }


    /*!
     *  \brief  deinit: dumping resource tracing result if enabled
     */
    void deinit_dump_trace_resource() override {
        pos_retval_t retval = POS_SUCCESS;
        std::string trace_dir, apicxt_dir, resource_dir;
        uint64_t i;
        POSHandleManager<POSHandle>* hm;
        POSHandle *handle;
        POSAPIContext_QE *wqe;
        std::vector<POSAPIContext_QE*> wqes;

        POS_LOG_C("dumping trace resource result...");

        // create directory
        retval = this->_ws->ws_conf.get(POSWorkspaceConf::kRuntimeTraceDir, trace_dir);
        if(unlikely(retval != POS_SUCCESS)){
            POS_WARN_C("failed to obtain directory to store trace result, failed to dump");
            goto exit;
        }
        trace_dir += std::string("/")
                    + std::to_string(this->_cxt.pid)
                    + std::string("-")
                    + std::to_string(this->_ws->tsc_timer.get_tsc());
        apicxt_dir = trace_dir + std::string("/apicxt/");
        resource_dir = trace_dir + std::string("/resource/");
        if (std::filesystem::exists(trace_dir)) { std::filesystem::remove_all(trace_dir); }
        try {
            std::filesystem::create_directories(apicxt_dir);
            std::filesystem::create_directories(resource_dir);
        } catch (const std::filesystem::filesystem_error& e) {
            POS_WARN_C("failed to create directory to store trace result, failed to dump");
            goto exit;
        }
        POS_BACK_LINE;
        POS_LOG_C("dumping trace resource result to %s...", trace_dir.c_str());

        // dumping API context
        wqes.clear();
        this->template poll_q<kPOS_QueueDirection_ParserLocal, kPOS_QueueType_ApiCxt_Trace_WQ>(&wqes);
        for(i=0; i<wqes.size(); i++){
            POS_CHECK_POINTER(wqe = wqes[i]);
            wqe->persist(apicxt_dir);
        }

        // dumping resources
        for(auto &handle_id : this->_ws->handle_type_idx){
            POS_CHECK_POINTER(
                hm = pos_get_client_typed_hm(this, handle_id, POSHandleManager<POSHandle>)
            );
            for(i=0; i<hm->get_nb_handles(); i++){
                POS_CHECK_POINTER(handle = hm->get_handle_by_id(i));
                retval = handle->persist_without_state_sync(resource_dir);
                if(unlikely(POS_SUCCESS != retval)){
                    POS_WARN_C("failed to dump status of handle");
                    retval = POS_FAILED;
                    goto exit;
                }
            }
        }

        POS_BACK_LINE;
        POS_LOG_C("dumping trace resource result to %s [done]", trace_dir.c_str());

    exit:
        ;
    }


    #if POS_CONF_EVAL_MigrOptLevel > 0

    /*! 
     *  \brief  remote malloc memories during migration
     */
    void __TMP__migration_remote_malloc(){
        pos_retval_t retval = POS_SUCCESS;

    exit:
        ;
    }

    /*! 
     *  \brief  precopy stateful handles to another device during migration
     */
    void __TMP__migration_precopy() override {
        POSHandleManager_CUDA_Memory *hm_memory;
        POSHandle_CUDA_Memory *memory_handle;
        uint64_t i, nb_handles;
        cudaError_t cuda_rt_retval;
        typename std::set<POSHandle_CUDA_Memory*>::iterator memory_handle_set_iter;

        uint64_t nb_precopy_handle = 0, precopy_size = 0; 
        uint64_t nb_host_handle = 0, host_handle_size = 0;

        hm_memory = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Memory, POSHandleManager_CUDA_Memory);
        POS_CHECK_POINTER(hm_memory);

        std::set<POSHandle_CUDA_Memory*>& modified_handles = hm_memory->get_modified_handles();
        if(likely(modified_handles.size() > 0)){
            for(memory_handle_set_iter = modified_handles.begin(); memory_handle_set_iter != modified_handles.end(); memory_handle_set_iter++){
                memory_handle = *memory_handle_set_iter;
                POS_CHECK_POINTER(memory_handle);

                // skip duplicated buffers
                if(hm_memory->is_host_stateful_handle(memory_handle)){
                    this->migration_ctx.__TMP__host_handles.insert(memory_handle);
                    nb_host_handle += 1;
                    host_handle_size += memory_handle->state_size;
                    // we still copy it, and deduplicate on the CPU-side
                    // continue;
                }

                cuda_rt_retval = cudaMemcpyPeerAsync(
                    /* dst */ memory_handle->remote_server_addr,
                    /* dstDevice */ 1,
                    /* src */ memory_handle->server_addr,
                    /* srcDevice */ 0,
                    /* count */ memory_handle->state_size,
                    /* stream */ (cudaStream_t)(this->worker->_migration_precopy_stream_id)
                );
                if(unlikely(cuda_rt_retval != CUDA_SUCCESS)){
                    POS_WARN("failed to p2p copy memory: server_addr(%p), state_size(%lu)", memory_handle->server_addr, memory_handle->state_size);
                    continue;
                }
            
                cuda_rt_retval = cudaStreamSynchronize((cudaStream_t)(this->worker->_migration_precopy_stream_id));
                if(unlikely(cuda_rt_retval != CUDA_SUCCESS)){
                    POS_WARN("failed to synchronize p2p copy memory: server_addr(%p), state_size(%lu)", memory_handle->server_addr, memory_handle->state_size);
                    continue;
                }

                this->migration_ctx.precopy_handles.insert(memory_handle);
                nb_precopy_handle += 1;
                precopy_size += memory_handle->state_size;
            }
        }

        nb_handles = hm_memory->get_nb_handles();
        hm_memory->clear_modified_handle();

    exit:
        ;
    }
    
    /*! 
     *  \brief  deltacopy stateful handles to another device during migration
     */
    void __TMP__migration_deltacopy() override {
        pos_retval_t retval = POS_SUCCESS;
        POSHandleManager_CUDA_Memory *hm_memory;
        typename std::set<POSHandle*>::iterator set_iter;
        POSHandle *memory_handle;
        uint64_t nb_deltacopy_handle = 0, deltacopy_size = 0;
        cudaError_t cuda_rt_retval;

        hm_memory = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Memory, POSHandleManager_CUDA_Memory);
        POS_CHECK_POINTER(hm_memory);

        for(set_iter = this->migration_ctx.invalidated_handles.begin(); set_iter != this->migration_ctx.invalidated_handles.end(); set_iter++){
            memory_handle = *set_iter;

            // skip duplicated buffers
            if(hm_memory->is_host_stateful_handle((POSHandle_CUDA_Memory*)(memory_handle))){
                continue;
            }

            cuda_rt_retval = cudaMemcpyPeerAsync(
                /* dst */ memory_handle->remote_server_addr,
                /* dstDevice */ 1,
                /* src */ memory_handle->server_addr,
                /* srcDevice */ 0,
                /* count */ memory_handle->state_size,
                /* stream */ (cudaStream_t)(this->worker->_migration_precopy_stream_id)
            );
            if(unlikely(cuda_rt_retval != CUDA_SUCCESS)){
                POS_WARN("failed to p2p delta copy memory: server_addr(%p), state_size(%lu)", memory_handle->server_addr, memory_handle->state_size);
                continue;
            }

            cuda_rt_retval = cudaStreamSynchronize((cudaStream_t)(this->worker->_migration_precopy_stream_id));
            if(unlikely(cuda_rt_retval != CUDA_SUCCESS)){
                POS_WARN("failed to synchronize p2p delta copy memory: server_addr(%p), state_size(%lu)", memory_handle->server_addr, memory_handle->state_size);
                continue;
            }

            nb_deltacopy_handle += 1;
            deltacopy_size += memory_handle->state_size;
        }

    exit:
        ;
    }

    void __TMP__migration_tear_context(bool do_tear_module) override {
        POSHandleManager_CUDA_Context *hm_context;
        POSHandleManager_cuBLAS_Context *hm_cublas;
        POSHandleManager_CUDA_Stream *hm_stream;
        POSHandleManager_CUDA_Module *hm_module;
        POSHandleManager_CUDA_Function *hm_function;

        POSHandle_CUDA_Context *context_handle;
        POSHandle_cuBLAS_Context *cublas_handle;
        POSHandle_CUDA_Stream *stream_handle;
        POSHandle_CUDA_Module *module_handle;
        POSHandle_CUDA_Function *function_handle;

        uint64_t i, nb_handles;

        hm_context = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Context, POSHandleManager_CUDA_Context);
        POS_CHECK_POINTER(hm_context);
        hm_cublas = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_cuBLAS_Context, POSHandleManager_cuBLAS_Context);
        POS_CHECK_POINTER(hm_cublas);
        hm_stream = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Stream, POSHandleManager_CUDA_Stream);
        POS_CHECK_POINTER(hm_stream);
        hm_module = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Module, POSHandleManager_CUDA_Module);
        POS_CHECK_POINTER(hm_module);
        hm_function = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Function, POSHandleManager_CUDA_Function);
        POS_CHECK_POINTER(hm_function);

        POS_LOG("destory cublas")
        nb_handles = hm_cublas->get_nb_handles();
        for(i=0; i<nb_handles; i++){
            cublas_handle = hm_cublas->get_handle_by_id(i);
            POS_CHECK_POINTER(cublas_handle);
            if(cublas_handle->status == kPOS_HandleStatus_Active){
                cublasDestroy_v2((cublasHandle_t)(cublas_handle->server_addr));
                cublas_handle->status = kPOS_HandleStatus_Broken;
            }
        }

        // destory streams
        POS_LOG("destory streams")
        nb_handles = hm_stream->get_nb_handles();
        for(i=0; i<nb_handles; i++){
            stream_handle = hm_stream->get_handle_by_id(i);
            POS_CHECK_POINTER(stream_handle);
            if(stream_handle->status == kPOS_HandleStatus_Active){
                cudaStreamDestroy((cudaStream_t)(stream_handle->server_addr));
                stream_handle->status = kPOS_HandleStatus_Broken;
            }
        }

        if(do_tear_module){
            POS_LOG("modules & functions")
            nb_handles = hm_module->get_nb_handles();
            for(i=0; i<nb_handles; i++){
                module_handle = hm_module->get_handle_by_id(i);
                POS_CHECK_POINTER(module_handle);
                if(module_handle->status == kPOS_HandleStatus_Active){
                    cuModuleUnload((CUmodule)(module_handle->server_addr));
                    module_handle->status = kPOS_HandleStatus_Broken;
                }
            }

            nb_handles = hm_function->get_nb_handles();
            for(i=0; i<nb_handles; i++){
                function_handle = hm_function->get_handle_by_id(i);
                if(function_handle->status == kPOS_HandleStatus_Active){
                    function_handle->status = kPOS_HandleStatus_Broken;
                }
            }
        }
    }

    void __TMP__migration_restore_context(bool do_restore_module){
        POSHandleManager_CUDA_Context *hm_context;
        POSHandleManager_cuBLAS_Context *hm_cublas;
        POSHandleManager_CUDA_Stream *hm_stream;
        POSHandleManager_CUDA_Module *hm_module;
        POSHandleManager_CUDA_Function *hm_function;

        POSHandle_CUDA_Context *context_handle;
        POSHandle_cuBLAS_Context *cublas_handle;
        POSHandle_CUDA_Stream *stream_handle;
        POSHandle_CUDA_Module *module_handle;
        POSHandle_CUDA_Function *function_handle;

        uint64_t i, nb_handles;

        hm_context = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Context, POSHandleManager_CUDA_Context);
        POS_CHECK_POINTER(hm_context);
        hm_cublas = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_cuBLAS_Context, POSHandleManager_cuBLAS_Context);
        POS_CHECK_POINTER(hm_cublas);
        hm_stream = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Stream, POSHandleManager_CUDA_Stream);
        POS_CHECK_POINTER(hm_stream);
        hm_module = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Module, POSHandleManager_CUDA_Module);
        POS_CHECK_POINTER(hm_module);
        hm_function = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Function, POSHandleManager_CUDA_Function);
        POS_CHECK_POINTER(hm_function);

        // restore cublas
        nb_handles = hm_cublas->get_nb_handles();
        for(i=0; i<nb_handles; i++){
            cublas_handle = hm_cublas->get_handle_by_id(i);
            cublas_handle->restore();
        }

        // restore streams
        nb_handles = hm_stream->get_nb_handles();
        for(i=0; i<nb_handles; i++){
            stream_handle = hm_stream->get_handle_by_id(i);
            stream_handle->restore();
        }

        // restore modules & functions
        if(do_restore_module){
            nb_handles = hm_module->get_nb_handles();
            for(i=0; i<nb_handles; i++){
                module_handle = hm_module->get_handle_by_id(i);
                module_handle->restore();
            }

            nb_handles = hm_function->get_nb_handles();
            for(i=0; i<nb_handles; i++){
                function_handle = hm_function->get_handle_by_id(i);
                function_handle->restore();
            }
        }
    }

    void __TMP__migration_ondemand_reload() override {
        pos_retval_t retval = POS_SUCCESS;
        typename std::set<POSHandle*>::iterator set_iter;
        POSHandle *memory_handle;
        cudaError_t cuda_rt_retval;

        uint64_t nb_handles = 0, reload_size = 0;

        for(
            set_iter = this->migration_ctx.__TMP__host_handles.begin();
            set_iter != this->migration_ctx.__TMP__host_handles.end(); 
            set_iter++
        ){
            memory_handle = *set_iter;
            POS_CHECK_POINTER(memory_handle);

            if(unlikely(POS_SUCCESS != memory_handle->reload_state(this->worker->_migration_precopy_stream_id))){
                POS_WARN("failed to reload state of handle within on-demand reload thread: server_addr(%p)", memory_handle->server_addr);
            } else {
                memory_handle->state_status = kPOS_HandleStatus_StateReady;
                nb_handles += 1;
                reload_size += memory_handle->state_size;
            }
        }
    }

    void __TMP__migration_allcopy() override {
        POSHandleManager_CUDA_Memory *hm_memory;
        uint64_t i, nb_handles;
        uint64_t dump_size = 0;
        POSHandle_CUDA_Memory *memory_handle;

        hm_memory = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Memory, POSHandleManager_CUDA_Memory);
        POS_CHECK_POINTER(hm_memory);

        nb_handles = hm_memory->get_nb_handles();
        for(i=0; i<nb_handles; i++){
            memory_handle = hm_memory->get_handle_by_id(i);
            if(unlikely(memory_handle->status != kPOS_HandleStatus_Active)){
                continue;
            }

            if(unlikely(POS_SUCCESS != memory_handle->checkpoint_commit_sync(
                /* version_id */ memory_handle->latest_version,
                /* ckpt_dir */ "",
                /* stream_id */ 0
            ))){
                POS_WARN(
                    "failed to checkpoint handle: server_addr(%p), state_size(%lu)",
                    memory_handle->server_addr,
                    memory_handle->state_size
                );
            }
            
            dump_size += memory_handle->state_size;
        }
    }

    void __TMP__migration_allreload() override {
        POSHandleManager_CUDA_Memory *hm_memory;
        uint64_t i, nb_handles;
        uint64_t reload_size = 0;
        POSHandle_CUDA_Memory *memory_handle;

        hm_memory = pos_get_client_typed_hm(this, kPOS_ResourceTypeId_CUDA_Memory, POSHandleManager_CUDA_Memory);
        POS_CHECK_POINTER(hm_memory);

        nb_handles = hm_memory->get_nb_handles();
        for(i=0; i<nb_handles; i++){
            memory_handle = hm_memory->get_handle_by_id(i);
            if(unlikely(memory_handle->status != kPOS_HandleStatus_Active)){
                continue;
            }

            if(unlikely(POS_SUCCESS != memory_handle->reload_state(/* stream_id */ 0))){
                POS_WARN(
                    "failed to reload state of handle: server_addr(%p), state_size(%lu)",
                    memory_handle->server_addr,
                    memory_handle->state_size
                );
            }
            
            reload_size += memory_handle->state_size;
        }
    }

    #endif // POS_CONF_EVAL_MigrOptLevel > 0

 protected:
    /*!
     *  \brief  obtain all resource type indices of this client
     *  \return all resource type indices of this client
     */
    std::set<pos_resource_typeid_t> __get_resource_idx() override {
        return  std::set<pos_resource_typeid_t>({
            kPOS_ResourceTypeId_CUDA_Context,
            kPOS_ResourceTypeId_CUDA_Module,
            kPOS_ResourceTypeId_CUDA_Function,
            kPOS_ResourceTypeId_CUDA_Var,
            kPOS_ResourceTypeId_CUDA_Device,
            kPOS_ResourceTypeId_CUDA_Memory,
            kPOS_ResourceTypeId_CUDA_Stream,
            kPOS_ResourceTypeId_CUDA_Event,
            kPOS_ResourceTypeId_cuBLAS_Context
        });
    }


 private:
    pos_client_cxt_CUDA _cxt_CUDA;

    /*!
     *  \brief  export the metadata of functions
     */
    void __dump_hm_cuda_functions() {
        uint64_t nb_functions, i;
        POSHandleManager_CUDA_Function *hm_function;
        POSHandle_CUDA_Function *function_handle;
        std::ofstream output_file;
        std::string dump_content;

        auto dump_function_metas = [](POSHandle_CUDA_Function* function_handle) -> std::string {
            std::string output_str("");
            std::string delimiter("|");
            uint64_t i;
            
            POS_CHECK_POINTER(function_handle);

            // mangled name of the kernel
            output_str += function_handle->name + std::string(delimiter);
            
            // signature of the kernel
            output_str += function_handle->signature + std::string(delimiter);

            // number of paramters
            output_str += std::to_string(function_handle->nb_params);
            output_str += std::string(delimiter);

            // parameter offsets
            for(i=0; i<function_handle->nb_params; i++){
                output_str += std::to_string(function_handle->param_offsets[i]);
                output_str += std::string(delimiter);
            }

            // parameter sizes
            for(i=0; i<function_handle->nb_params; i++){
                output_str += std::to_string(function_handle->param_sizes[i]);
                output_str += std::string(delimiter);
            }

            // input paramters
            output_str += std::to_string(function_handle->input_pointer_params.size());
            output_str += std::string(delimiter);
            for(i=0; i<function_handle->input_pointer_params.size(); i++){
                output_str += std::to_string(function_handle->input_pointer_params[i]);
                output_str += std::string(delimiter);
            }

            // output paramters
            output_str += std::to_string(function_handle->output_pointer_params.size());
            output_str += std::string(delimiter);
            for(i=0; i<function_handle->output_pointer_params.size(); i++){
                output_str += std::to_string(function_handle->output_pointer_params[i]);
                output_str += std::string(delimiter);
            }

            // inout parameters
            output_str += std::to_string(function_handle->inout_pointer_params.size());
            output_str += std::string(delimiter);
            for(i=0; i<function_handle->inout_pointer_params.size(); i++){
                output_str += std::to_string(function_handle->inout_pointer_params[i]);
                output_str += std::string(delimiter);
            }

            // suspicious paramters
            output_str += std::to_string(function_handle->suspicious_params.size());
            output_str += std::string(delimiter);
            for(i=0; i<function_handle->suspicious_params.size(); i++){
                output_str += std::to_string(function_handle->suspicious_params[i]);
                output_str += std::string(delimiter);
            }

            // has verified suspicious paramters
            if(function_handle->has_verified_params){
                output_str += std::string("1") + std::string(delimiter);

                // inout paramters
                output_str += std::to_string(function_handle->confirmed_suspicious_params.size());
                output_str += std::string(delimiter);
                for(i=0; i<function_handle->confirmed_suspicious_params.size(); i++){
                    output_str += std::to_string(function_handle->confirmed_suspicious_params[i].first);    // param_index
                    output_str += std::string(delimiter);
                    output_str += std::to_string(function_handle->confirmed_suspicious_params[i].second);   // offset
                    output_str += std::string(delimiter);
                }
            } else {
                output_str += std::string("0") + std::string(delimiter);
            }

            // cbank parameters
            output_str += std::to_string(function_handle->cbank_param_size);

            return output_str;
        };

        // if we have already save the kernels, we can skip
        // if(likely(this->_cxt.is_load_kernel_from_cache == true)){
        //     goto exit;
        // }

        hm_function 
            = (POSHandleManager_CUDA_Function*)(this->handle_managers[kPOS_ResourceTypeId_CUDA_Function]);
        POS_CHECK_POINTER(hm_function);

        output_file.open(this->_cxt.kernel_meta_path.c_str(), std::fstream::in | std::fstream::out | std::fstream::app);

        nb_functions = hm_function->get_nb_handles();
        for(i=0; i<nb_functions; i++){
            POS_CHECK_POINTER(function_handle = hm_function->get_handle_by_id(i));
            output_file << dump_function_metas(function_handle) << std::endl;
        }

        output_file.close();
        POS_LOG("finish dump kernel metadata to %s", this->_cxt.kernel_meta_path.c_str());

    exit:
        ;
    }
};
