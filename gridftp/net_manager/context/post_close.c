/*
 * Copyright 1999-2014 University of Chicago
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file post_close.c
 * @brief globus_net_manager_context_post_close()
 */

#include "globus_net_manager_context.h"

/**
 * @brief Call Context Post Close
 * @ingroup globus_net_manager_context
 * @details
 * Call the chain of post_close() functions in a context with the given
 * task_id, transport, contact information, and attributes. If any manager in
 * the context returns an error, the subsequent managers will not be called.
 *
 * @param [in] context
 *     The context to call functions in.
 * @param [in] task_id
 *     The task_id associated with this network operation.
 * @param [in] transport
 *     The transport driver associated with this network operation.
 * @param [in] local_contact
 *     The string representation of the local address of the socket.
 * @param [in] remote_contact
 *     The string representation of the remote address of the socket
 * @param [in] attr_array
 *     The set of attributes to pass to the managers.
 *
 * @return
 *     On success, this function returns GLOBUS_SUCCESS. If any manager
 *     returns an error, the post_close calls stop and the error is returned
 *     to the caller.
 */
globus_result_t
globus_net_manager_context_post_close(
    globus_net_manager_context_t        context,
    const char                         *task_id,
    const char                         *transport,
    const char                         *local_contact,
    const char                         *remote_contact,
    const globus_net_manager_attr_t    *attr_array)
{
    globus_i_net_manager_context_t *    ctx = context;
    globus_list_t *                     list;
    globus_result_t                     result = GLOBUS_SUCCESS;
    globus_i_net_manager_context_entry_t * ent;

    if(!ctx || !task_id || !transport || !attr_array ||
        !local_contact || !remote_contact)
    {
        result = GlobusNetManagerErrorParameter("No parameter may be NULL.");
        goto error_bad_args;
    }

    for(list = ctx->managers; 
        !globus_list_empty(list) && result == GLOBUS_SUCCESS; 
        list = globus_list_rest(list))
    {            
        ent = globus_list_first(list);
        
        if(ent->manager->post_close)
        {
            result = ent->manager->post_close(
                ent->manager,
                ent->attrs,
                task_id,
                transport,
                local_contact,
                remote_contact,
                attr_array);
            if(result != GLOBUS_SUCCESS)
            {
                result = GlobusNetManagerErrorManager(
                    result, ent->manager->name, "post_close");
            }                
        }
    }
    
    return result;

error_bad_args:
    return result;
}
/* globus_net_manager_context_post_close() */
