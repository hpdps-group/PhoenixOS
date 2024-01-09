#pragma once

#include <cuda_runtime_api.h>

#include "pos/include/common.h"
#include "pos/include/workspace.h"
#include "pos/include/client.h"
#include "pos/include/transport.h"
#include "pos/include/worker.h"
#include "pos/include/checkpoint.h"
#include "pos/cuda_impl/client.h"

#include "pos/cuda_impl/api_index.h"

namespace wk_functions {
    /* CUDA runtime functions */
    POS_WK_DECLARE_FUNCTIONS(cuda_malloc);
    POS_WK_DECLARE_FUNCTIONS(cuda_free);
    POS_WK_DECLARE_FUNCTIONS(cuda_launch_kernel);
    POS_WK_DECLARE_FUNCTIONS(cuda_memcpy_h2d);
    POS_WK_DECLARE_FUNCTIONS(cuda_memcpy_d2h);
    POS_WK_DECLARE_FUNCTIONS(cuda_memcpy_d2d);
    POS_WK_DECLARE_FUNCTIONS(cuda_memcpy_h2d_async);
    POS_WK_DECLARE_FUNCTIONS(cuda_memcpy_d2h_async);
    POS_WK_DECLARE_FUNCTIONS(cuda_memcpy_d2d_async);
    POS_WK_DECLARE_FUNCTIONS(cuda_set_device);
    POS_WK_DECLARE_FUNCTIONS(cuda_get_last_error);
    POS_WK_DECLARE_FUNCTIONS(cuda_get_error_string);
    POS_WK_DECLARE_FUNCTIONS(cuda_get_device_count);
    POS_WK_DECLARE_FUNCTIONS(cuda_get_device_properties);
    POS_WK_DECLARE_FUNCTIONS(cuda_get_device);
    POS_WK_DECLARE_FUNCTIONS(cuda_stream_synchronize);
    POS_WK_DECLARE_FUNCTIONS(cuda_stream_is_capturing);
    POS_WK_DECLARE_FUNCTIONS(cuda_event_create_with_flags);
    POS_WK_DECLARE_FUNCTIONS(cuda_event_destory);
    POS_WK_DECLARE_FUNCTIONS(cuda_event_record);

    /* CUDA driver functions */
    POS_WK_DECLARE_FUNCTIONS(cu_module_load_data);
    POS_WK_DECLARE_FUNCTIONS(cu_module_get_function);
    POS_WK_DECLARE_FUNCTIONS(cu_module_get_global);
    POS_WK_DECLARE_FUNCTIONS(cu_device_primary_ctx_get_state);

    /* cuBLAS functions */
    POS_WK_DECLARE_FUNCTIONS(cublas_create);
    POS_WK_DECLARE_FUNCTIONS(cublas_set_stream);
    POS_WK_DECLARE_FUNCTIONS(cublas_set_math_mode);
    POS_WK_DECLARE_FUNCTIONS(cublas_sgemm);
} // namespace rt_functions

/*!
 *  \brief  POS Worker (CUDA Implementation)
 */
template<class T_POSTransport>
class POSWorker_CUDA : public POSWorker<T_POSTransport, POSClient_CUDA> {
 public:
    POSWorker_CUDA(POSWorkspace<T_POSTransport, POSClient_CUDA>* ws) 
        : POSWorker<T_POSTransport, POSClient_CUDA>(ws), sample_ckpt(0) {
        #if POS_CKPT_OPT_LEVAL == 2
            if(unlikely(cudaSuccess != cudaStreamCreate(&_ckpt_stream))){
                POS_ERROR_C_DETAIL("failed to create CUDA stream for checkpoint");
            }
        #endif
        }
    ~POSWorker_CUDA(){
        #if POS_CKPT_OPT_LEVAL == 2
            if(unlikely(cudaSuccess != cudaStreamDestroy(_ckpt_stream))){
                POS_ERROR_C_DETAIL("failed to destroy CUDA stream for checkpoint");
            }
        #endif
    };
 
 protected:
    uint64_t sample_ckpt;

    /*!
     *  \brief  naive implementation of the checkpoint procedure
     *  \note   this procedure checkpoints all memory handles, stores all checkpointing
     *          history of all buffers, cause (1) long checkpoint latency and (2) large
     *          checkpointing memory consumption
     * \param   wqe WQ element of the checkpoint op
     */
    pos_retval_t __checkpoint_naive(POSAPIContext_QE_ptr wqe) {
        pos_retval_t retval = POS_SUCCESS;
        cudaError_t cuda_rt_retval;
        uint64_t nb_handles;
        POSClient_CUDA *client;
        POSHandle_ptr handle;
        POSHandleManager<POSHandle>* hm;
        POSCheckpointSlot_ptr ckpt_slot;
        uint64_t i;

        POS_CHECK_POINTER(wqe);

        client = (POSClient_CUDA*)(wqe->client);
        POS_CHECK_POINTER(client);

        wqe->nb_ckpt_handles = 0;
        wqe->ckpt_size = 0;
        wqe->ckpt_memory_consumption = 0;
        
        for(auto &stateful_handle_id : this->_ws->stateful_handle_type_idx){
            hm = client->handle_managers[stateful_handle_id];
            POS_CHECK_POINTER(hm);
            nb_handles = hm->get_nb_handles();
            for(i=0; i<nb_handles; i++){
                handle = hm->get_handle_by_id(i);
                POS_CHECK_POINTER(handle.get());

                retval = handle->checkpoint(
                    /* version_id */ wqe->dag_vertex_id,
                    /* stream_id */ 0
                );
                if(unlikely(POS_SUCCESS != retval)){
                    POS_WARN_C("failed to checkpoint handle");
                    retval = POS_FAILED;
                    goto exit;
                }

                wqe->nb_ckpt_handles += 1;
                wqe->ckpt_size += handle->state_size;
            }
        }

        // make sure the checkpoint is finished
        // TODO: make this to be more generic?
        cuda_rt_retval = cudaStreamSynchronize(0);
        if(unlikely(cuda_rt_retval != cudaSuccess)){
            POS_WARN_C("failed to synchronize after checkpointing");
            retval = POS_FAILED;
            goto exit;
        }

    exit:
        // collect statistics
        for(auto &stateful_handle_id : this->_ws->stateful_handle_type_idx){
            hm = client->handle_managers[stateful_handle_id];
            POS_CHECK_POINTER(hm);
            nb_handles = hm->get_nb_handles();
            for(i=0; i<nb_handles; i++){
                handle = hm->get_handle_by_id(i);
                POS_CHECK_POINTER(handle.get());
                wqe->ckpt_memory_consumption += handle->ckpt_bag->get_memory_consumption();
            }
        }

        return retval;
    }

    /*!
     *  \brief  level-1 optimizing implementation of the checkpoint procedure
     *  \note   this procedure checkpoints only those memory handles that been modified
     *          since last checkpointing
     * \param   wqe WQ element of the checkpoint op
     */
    pos_retval_t __checkpoint_o1(POSAPIContext_QE_ptr wqe) {
        uint64_t i;
        std::vector<POSHandleView_t>* handle_views;
        uint64_t nb_handles;
        POSClient_CUDA *client;
        POSHandle_ptr handle;
        POSHandleManager<POSHandle>* hm;
        cudaError_t cuda_rt_retval;
        POSCheckpointSlot_ptr ckpt_slot;
        pos_retval_t retval = POS_SUCCESS;

        POS_CHECK_POINTER(wqe);

        wqe->nb_ckpt_handles = 0;
        wqe->ckpt_size = 0;
        wqe->ckpt_memory_consumption = 0;

        for(auto &stateful_handle_id : this->_ws->stateful_handle_type_idx){
            client = (POSClient_CUDA*)(wqe->client);
            POS_CHECK_POINTER(client);
            hm = client->handle_managers[stateful_handle_id];
            POS_CHECK_POINTER(hm);

            // we only checkpoint those handles that been modified since last checkpointing
            if(likely(wqe->handle_view_map.count(stateful_handle_id) == 0)){
                continue;
            }
            POS_CHECK_POINTER(handle_views = wqe->handle_view_map[stateful_handle_id]);

            for(i=0; i<handle_views->size(); i++){
                handle = (*handle_views)[i].handle;
                POS_CHECK_POINTER(handle.get());

                retval = handle->checkpoint(
                    /* version_id */ wqe->dag_vertex_id,
                    /* stream_id */ 0
                );
                if(unlikely(POS_SUCCESS != retval)){
                    POS_WARN_C("failed to checkpoint handle");
                    retval = POS_FAILED;
                    goto exit;
                }

                wqe->nb_ckpt_handles += 1;
                wqe->ckpt_size += handle->state_size;
            }
        }

        // make sure the checkpoint is finished
        cuda_rt_retval = cudaStreamSynchronize(0);
        if(unlikely(cuda_rt_retval != cudaSuccess)){
            POS_WARN_C("failed to synchronize after checkpointing");
            retval = POS_FAILED;
            goto exit;
        }

    exit:
        // collect statistics
        for(auto &stateful_handle_id : this->_ws->stateful_handle_type_idx){
            hm = client->handle_managers[stateful_handle_id];
            POS_CHECK_POINTER(hm);
            nb_handles = hm->get_nb_handles();
            for(i=0; i<nb_handles; i++){
                handle = hm->get_handle_by_id(i);
                POS_CHECK_POINTER(handle.get());
                wqe->ckpt_memory_consumption += handle->ckpt_bag->get_memory_consumption();
            }
        }

        // POS_LOG_C(
        //     "checkpointed %lu handles, memory consumption: %lu",
        //     wqe->nb_ckpt_handles,
        //     wqe->ckpt_memory_consumption
        // );

        return retval;
    }

    /*!
     *  \brief  checkpoint procedure, should be implemented by each platform
     *  \param  wqe     the checkpoint op
     *  \return POS_SUCCESS for successfully checkpointing
     */
    pos_retval_t checkpoint(POSAPIContext_QE_ptr wqe) override {
        #if POS_CKPT_OPT_LEVAL == 1
            return __checkpoint_o1(wqe);
        #elif POS_CKPT_OPT_LEVAL == 2
            POS_ERROR_C_DETAIL("shouldn't invoke this function");
        #else // POS_CKPT_OPT_LEVAL == 0
            return POS_SUCCESS;
        #endif
    }

    /*!
     *  \brief  checkpoint procedure, should be implemented by each platform
     *  \note   this function will be invoked by level-2 ckpt
     *  \param  wqe     the checkpoint op
     *  \param  handles pointer to vector that stores handles to be checkpointed
     *  \return POS_SUCCESS for successfully checkpointing
     */
    pos_retval_t checkpoint_async(POSAPIContext_QE_ptr wqe, std::vector<POSHandle_ptr>* handles) override {
        pos_retval_t retval = POS_SUCCESS;
        POSHandle_ptr handle;
        uint64_t i;

        POS_CHECK_POINTER(handles);

        wqe->nb_ckpt_handles = 0;
        wqe->ckpt_size = 0;

        // how to collect memory consumption data?
        wqe->ckpt_memory_consumption = 0;

        for(i=0; i<handles->size(); i++){
            handle = (*handles)[i];
            POS_CHECK_POINTER(handle.get());

            retval = handle->checkpoint(
                /* version_id */ wqe->dag_vertex_id,
                /* stream_id */ (uint64_t)(_ckpt_stream)
            );
            if(unlikely(POS_SUCCESS != retval)){
                POS_WARN_C("failed to checkpoint handle");
                retval = POS_FAILED;
                goto exit;
            }

            wqe->nb_ckpt_handles += 1;
            wqe->ckpt_size += handle->state_size;
        }

    exit:
        return retval; 
    }

    pos_retval_t checkpoint_join() override {
        cudaStreamSynchronize(_ckpt_stream);
    }

    /*!
     *  \brief  generate overlap ckpt scheme
     *  \note   this function will be invoked by level-2 ckpt
     *  \param  wqe             the checkpoint op
     *  \param  nb_pending_op   number of pending ops following the ckpt op
     *  \param  ckpt_scheme     pointer to the generated checkpoint overlap scheme
     *  \return POS_SUCCESS for successfully generation
     */
    pos_retval_t generate_overlap_ckpt_scheme(
        POSAPIContext_QE_ptr wqe,
        uint64_t nb_pending_op,
        pos_ckpt_overlap_scheme_t* ckpt_scheme
    ) override {
        uint64_t i;
        pos_retval_t retval = POS_SUCCESS;
        std::vector<POSHandleView_t>* handle_views;
        POSHandle_ptr handle;
        POSClient_CUDA* client;
        std::vector<POSHandle_ptr> remain_handles;
        cudaError_t cuda_rt_retval;

        std::vector<uint64_t> handle_modified_position;
        uint64_t deadline_position, relative_deadline_position;

        POS_CHECK_POINTER(wqe.get());
        POS_CHECK_POINTER(ckpt_scheme);
        POS_CHECK_POINTER(client = wqe->client);

        ckpt_scheme->refresh(nb_pending_op);

        // extract all modified handles
        for(auto& stateful_handle_id : this->_ws->stateful_handle_type_idx){
            // we only checkpoint those handles that been modified since last checkpointing
            if(likely(wqe->handle_view_map.count(stateful_handle_id) == 0)){
                continue;
            }
            POS_CHECK_POINTER(handle_views = wqe->handle_view_map[stateful_handle_id]);

            for(i=0; i<handle_views->size(); i++){
                handle = (*handle_views)[i].handle;
                POS_CHECK_POINTER(handle.get());
                remain_handles.push_back(handle);
            }
        }

        // distribute the checkpoint of these modified handles to each following ops
        for(auto& remain_handle : remain_handles){
            // first we query where the handle will be modified in the next following ops
            retval != client->dag.get_handle_modified_position(
                /* handle_id */ remain_handle->dag_vertex_id, 
                /* start_op_id */ wqe->dag_vertex_id + 1,
                /* end_op_id */ wqe->dag_vertex_id + nb_pending_op,
                /* positions */ &handle_modified_position
            );
            if(unlikely(retval != POS_SUCCESS)){
                POS_WARN_C_DETAIL(
                    "failed to obtain modified position of handle: type_id(%u), client_addr(%p), server_addr(%p)",
                    remain_handle->resource_type_id,
                    remain_handle->client_addr,
                    remain_handle->server_addr
                );
                goto exit;
            }

            /*!
             *  \note   obtain the checkpoint deadline
             *          (i.e., the final position we can checkpoint this handle in overlap manner)
             */
            if(unlikely(handle_modified_position.size() == 0)){
                deadline_position = wqe->dag_vertex_id + (nb_pending_op-1);
            } else {
                POS_ASSERT(handle_modified_position[0] > wqe->dag_vertex_id); // must be...
                deadline_position = handle_modified_position[0] - 1;                
            }
            relative_deadline_position = deadline_position - wqe->dag_vertex_id;

            ckpt_scheme->add_new_handle_for_distribute(relative_deadline_position, remain_handle);
        }

        ckpt_scheme->schedule();
        
        // make sure the previous ops are done
        // TODO:    how to synchronize only current client's previous ops?
        //          cudaDeviceSynchronize will sync all clients' previous ops...
        // TODO:    we should move this logic to other function 
        cuda_rt_retval = cudaStreamSynchronize(0);
        if(unlikely(cuda_rt_retval != cudaSuccess)){
            POS_WARN_C_DETAIL("failed to synchronize");
            retval = POS_FAILED;
            goto exit;
        }

    exit:
        return retval;
    }

 private:
    cudaStream_t _ckpt_stream;

    /*!
     *  \brief      initialization of the worker daemon thread
     *  \example    for CUDA, one need to call API e.g. cudaSetDevice first to setup the context for a thread
     */
    pos_retval_t daemon_init() override {
        /*!
         *  \note   make sure the worker thread is bound to a CUDA context
         *          if we don't do this and use the driver API, it might be unintialized
         */
        if(cudaSetDevice(0) != cudaSuccess){
            POS_WARN_C_DETAIL("worker thread failed to invoke cudaSetDevice");
            return POS_FAILED; 
        }
        cudaDeviceSynchronize();

        return POS_SUCCESS; 
    }
    
    /*!
     *  \brief  insertion of worker functions
     *  \return POS_SUCCESS for succefully insertion
     */
    pos_retval_t init_wk_functions() override {
        this->_launch_functions.insert({
            /* CUDA runtime functions */
            {   CUDA_MALLOC,                    wk_functions::cuda_malloc::launch                       },
            {   CUDA_FREE,                      wk_functions::cuda_free::launch                         },
            {   CUDA_LAUNCH_KERNEL,             wk_functions::cuda_launch_kernel::launch                },
            {   CUDA_MEMCPY_HTOD,               wk_functions::cuda_memcpy_h2d::launch                   },
            {   CUDA_MEMCPY_DTOH,               wk_functions::cuda_memcpy_d2h::launch                   },
            {   CUDA_MEMCPY_DTOD,               wk_functions::cuda_memcpy_d2d::launch                   },
            {   CUDA_MEMCPY_HTOD_ASYNC,         wk_functions::cuda_memcpy_h2d_async::launch             },
            {   CUDA_MEMCPY_DTOH_ASYNC,         wk_functions::cuda_memcpy_d2h_async::launch             },
            {   CUDA_MEMCPY_DTOD_ASYNC,         wk_functions::cuda_memcpy_d2d_async::launch             },
            {   CUDA_SET_DEVICE,                wk_functions::cuda_set_device::launch                   },
            {   CUDA_GET_LAST_ERROR,            wk_functions::cuda_get_last_error::launch               },
            {   CUDA_GET_ERROR_STRING,          wk_functions::cuda_get_error_string::launch             },
            {   CUDA_GET_DEVICE_COUNT,          wk_functions::cuda_get_device_count::launch             },
            {   CUDA_GET_DEVICE_PROPERTIES,     wk_functions::cuda_get_device_properties::launch        },
            {   CUDA_GET_DEVICE,                wk_functions::cuda_get_device::launch                   },
            {   CUDA_STREAM_SYNCHRONIZE,        wk_functions::cuda_stream_synchronize::launch           },
            {   CUDA_STREAM_IS_CAPTURING,       wk_functions::cuda_stream_is_capturing::launch          },
            {   CUDA_EVENT_CREATE_WITH_FLAGS,   wk_functions::cuda_event_create_with_flags::launch      },
            {   CUDA_EVENT_DESTROY,             wk_functions::cuda_event_destory::launch                },
            {   CUDA_EVENT_RECORD,              wk_functions::cuda_event_record::launch                 },
            /* CUDA driver functions */
            {   rpc_cuModuleLoad,               wk_functions::cu_module_load_data::launch               },
            {   rpc_cuModuleGetFunction,        wk_functions::cu_module_get_function::launch            },
            {   rpc_register_var,               wk_functions::cu_module_get_global::launch              },
            {   rpc_cuDevicePrimaryCtxGetState, wk_functions::cu_device_primary_ctx_get_state::launch   },
            /* cuBLAS functions */
            {   rpc_cublasCreate,               wk_functions::cublas_create::launch                     },
            {   rpc_cublasSetStream,            wk_functions::cublas_set_stream::launch                 },
            {   rpc_cublasSetMathMode,          wk_functions::cublas_set_math_mode::launch              },
            {   rpc_cublasSgemm,                wk_functions::cublas_sgemm::launch                      },
        });
        POS_DEBUG_C("insert %lu worker launch functions", this->_launch_functions.size());

        this->_landing_functions.insert({
            /* CUDA runtime functions */
            {   CUDA_MALLOC,                    wk_functions::cuda_malloc::landing                      },
            {   CUDA_FREE,                      wk_functions::cuda_free::landing                        },
            {   CUDA_LAUNCH_KERNEL,             wk_functions::cuda_launch_kernel::landing               },
            {   CUDA_MEMCPY_HTOD,               wk_functions::cuda_memcpy_h2d::landing                  },
            {   CUDA_MEMCPY_DTOH,               wk_functions::cuda_memcpy_d2h::landing                  },
            {   CUDA_MEMCPY_DTOD,               wk_functions::cuda_memcpy_d2d::landing                  },
            {   CUDA_MEMCPY_HTOD_ASYNC,         wk_functions::cuda_memcpy_h2d_async::landing            },
            {   CUDA_MEMCPY_DTOH_ASYNC,         wk_functions::cuda_memcpy_d2h_async::landing            },
            {   CUDA_MEMCPY_DTOD_ASYNC,         wk_functions::cuda_memcpy_d2d_async::landing            },
            {   CUDA_SET_DEVICE,                wk_functions::cuda_set_device::landing                  },
            {   CUDA_GET_LAST_ERROR,            wk_functions::cuda_get_last_error::landing              },
            {   CUDA_GET_ERROR_STRING,          wk_functions::cuda_get_error_string::landing            },
            {   CUDA_GET_DEVICE_COUNT,          wk_functions::cuda_get_device_count::landing            },
            {   CUDA_GET_DEVICE_PROPERTIES,     wk_functions::cuda_get_device_properties::landing       },
            {   CUDA_GET_DEVICE,                wk_functions::cuda_get_device::landing                  },
            {   CUDA_STREAM_SYNCHRONIZE,        wk_functions::cuda_stream_synchronize::landing          },
            {   CUDA_STREAM_IS_CAPTURING,       wk_functions::cuda_stream_is_capturing::landing         },
            {   CUDA_EVENT_CREATE_WITH_FLAGS,   wk_functions::cuda_event_create_with_flags::landing     },
            {   CUDA_EVENT_DESTROY,             wk_functions::cuda_event_destory::landing               },
            {   CUDA_EVENT_RECORD,              wk_functions::cuda_event_record::landing                },
            /* CUDA driver functions */
            {   rpc_cuModuleLoad,               wk_functions::cu_module_load_data::landing              },
            {   rpc_cuModuleGetFunction,        wk_functions::cu_module_get_function::landing           },
            {   rpc_register_var,               wk_functions::cu_module_get_global::landing             },
            {   rpc_cuDevicePrimaryCtxGetState, wk_functions::cu_device_primary_ctx_get_state::landing  },
            /* cuBLAS functions */
            {   rpc_cublasCreate,               wk_functions::cublas_create::landing                    },
            {   rpc_cublasSetStream,            wk_functions::cublas_set_stream::landing                },
            {   rpc_cublasSetMathMode,          wk_functions::cublas_set_math_mode::landing             },
            {   rpc_cublasSgemm,                wk_functions::cublas_sgemm::landing                     }
        });
        POS_DEBUG_C("insert %lu worker landing functions", this->_landing_functions.size());

        return POS_SUCCESS;
    }
};

#include "pos/cuda_impl/worker/base.h"
