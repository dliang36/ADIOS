


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

