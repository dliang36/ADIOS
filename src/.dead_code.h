/*Writer Side */

/*

Printf's in the cod code below that I don't want to remove just yet
        printf(\"We have a read request!\\n\");\n\

                printf(\"Read request timestep: \\\%d\\t\\tData_timestep:\\\%d\\n\", time_req, data_timestep);\n\
                    printf(\"Discarding junk\\n\");\n\
                    printf(\"Submitting to target!\\n\");\n\
*/


// search a var list
FlexpathVarNode* 
queue_contains(FlexpathVarNode* queue, const char* name, int rank) 
{
    int compare_rank = 0;
    if (rank >= 0 ) {
        compare_rank = 1;
    }
    FlexpathVarNode* tmp = queue;
    while (tmp) {
        if (strcmp(tmp->varName, name)==0) {
            if (compare_rank) {
                if (tmp->rank == rank) {
                    return tmp;
                }
            } else {
                return tmp;
            }
        }
        tmp = tmp->next;
    }
    return NULL;
}
static char extern_string[] = "\
    int get_reader_timestep(cod_exec_context ec);\n\
    void gather_EVgroup(cod_exec_context ec);\n\
";

static cod_extern_entry extern_map[] = {
    {"get_reader_timestep", (void*)(long)cod_get_reader_timestep},
    {"gather_EVgroup", (void*)(long)cod_gather_EVgroup},
    {(void*)0, (void*)0}
};


/*Old copy buffer code, not sure we need it*/
    while (f->field_name != NULL)
    {
        FlexpathVarNode* a;
        if (!queue_contains(fileData->askedVars, f->field_name, rank)) {
            if ((a=queue_contains(fileData->formatVars, f->field_name, -1)) 
	       && 
	       (a->dimensions != NULL)) {
                FlexpathVarNode* dim = a->dimensions;
                while (dim) {
                    FMField *f2 = fileData->fm->format[0].field_list;
                    while (f2->field_name != NULL) {
                        if (strcmp(f2->field_name, dim->varName)==0) {
                            break;
                        }
                        f2++;
                    }
                    if (f2->field_name != NULL) {
                        memset(&temp[f2->field_offset], 0, f2->field_size);
                    }
                    dim = dim->next;
                }
                memset(&temp[f->field_offset], 0, f->field_size);
            }
        }   
        f++;
    }
    return temp;


void
process_close_msg(FlexpathWriteFileData *fileData, op_msg *close)
{

    fp_verbose(fileData, " process close msg, bridge %d\n", close->process_id);
    pthread_mutex_lock(&fileData->openMutex);
    fileData->openCount--;
    fileData->bridges[close->process_id].opened=0;
    fileData->bridges[close->process_id].condition = close->condition;
    pthread_mutex_unlock(&fileData->openMutex);

    /* if (fileData->openCount==0) { */
    /* 	FlexpathQueueNode* node = threaded_dequeue(&fileData->dataQueue,  */
    /* 						   &fileData->dataMutex,  */
    /* 						   &fileData->dataCondition, 1); */
    /* 	FMfree_var_rec_elements(fileData->fm->ioFormat, node->data); */

    /* 	drop_evgroup_msg *dropMsg = malloc(sizeof(drop_evgroup_msg)); */
    /* 	dropMsg->step = fileData->readerStep; */
    /* 	int wait = CMCondition_get(flexpathWriteData.cm, NULL); */
    /* 	dropMsg->condition = wait; */
    /* 	EVsubmit_general(fileData->dropSource, dropMsg, drop_evgroup_msg_free, fileData->attrs); */
    /* 	//EVsubmit_general(fileData->dropSource, dropMsg, NULL, fileData->attrs); */
    /* 	// Will have to change when not using ctrl thread. */
    /* 	CMCondition_wait(flexpathWriteData.cm,  wait); 		     */
		     
    /* 	fileData->readerStep++; */
    /* } */
		
    op_msg *ack = malloc(sizeof(op_msg));
    ack->file_name = strdup(fileData->name);
    ack->process_id = fileData->rank;
    ack->step = fileData->readerStep;
    ack->type = 2;
    ack->condition = close->condition;
    fileData->attrs = set_dst_rank_atom(fileData->attrs, close->process_id + 1);
    EVsubmit_general(fileData->opSource, 
		     ack, 
		     op_free, 
		     fileData->attrs);		
		
}


EVassoc_terminal_action(flexpathWriteData.cm, fileData->sinkStone, 
                        flush_format_list, flush_handler, fileData);


void
evgroup_msg_free(void *eventData, void *clientData)
{
    
    /* evgroup *msg = (evgroup*)eventData; */
    /* int num_vars = msg->num_vars; */
    /* int i; */
    /* for (i=0; i<num_vars; i++) { */
    /* 	free(msg->vars[i].offsets); */
    /* } */
    /* free(msg); */
}


/*Reader Side */

int
increment_index(int64_t ndim, uint64_t *dimen_array, uint64_t *index_array)
{
    ndim--;
    while (ndim >= 0) {
	index_array[ndim]++;
	if (index_array[ndim] < dimen_array[ndim]) {
	    return 1;
	}
	index_array[ndim] = 0;
	ndim--;
    }
    return 0;
}



//Need to make sure we don't already have the condition as it may come early
//We need to do this because evgroup messages can come early, but we need to wait for late ones
static int 
internal_flexpath_get_global_metadata_condition(flexpath_reader_file * fp, int timestep)
{
    //First we check to see if we already set the condition for the timestep
    timestep_condition_ptr curr = fp->global_metadata_conditions;
    while(curr)
    {
        if(curr->timestep == timestep)
            return curr->condition;
        
        curr = curr->next;
    }

    //If we didn't we are going to set it now
    curr = fp->global_metadata_conditions;
    while(curr && curr->next) curr = curr->next;
    
    if(!curr)
    {
        fp->global_metadata_conditions = calloc(1, sizeof(timestep_condition));
        curr = fp->global_metadata_condition;
    }
    else
    {
        curr->next = calloc(1, sizeof(timestep_condition));
        curr = curr->next;
    }
    
    curr->timestep = timestep;
    curr->condition = CMCondition_get(fp_read_data->cm, NULL);

    return curr->condition;
}


static int
internal_flexpath_signal_global_metadata_condition(flexpath_reader_file * fp, int timestep)
{
    int sig_condition = internal_flexpath_get_global_metadata_condition(fp, timestep);
    CMCondition_signal(fp_read_data->cm, sig_condition);
    return sig_condition;
}

static void 
internal_flexpath_remove_obsolete_globe_conditions(flexpath_reader_file * fp, int timestep)
{
    timestep_condition_ptr curr = fp->global_metadata_conditions;
    timestep_condition_ptr prev = NULL;
    while(curr)
    {
        if(curr->timestep < timestep)
        {
            timestep_condition_ptr temp;
            //Two cases: front of the linked list and not front of the linked list 
            if(!prev)
                fp->global_metadata_conditions = curr->next;
            else
                prev->next = curr->next;

            temp = curr->next;
            free(curr);
            curr = temp;
        }
        else
        {
            curr = curr->next;
        }
    }
}

static int
internal_flexpath_wait_global_metadata_condition(flexpath_reader_file * fp, int timestep)
{
    //Remove old conditions because they will be sent in order and we wait for them in order
    internal_flexpath_remove_obsolete_globe_conditions(fp, timestep);    
    int sig_condition = internal_flexpath_get_global_metadata_condition(fp, timestep);
    CMCondition_wait(fp_read_data->cm, sig_condition);
}

