/*
    read_flexpath.c
    Goal: to create evpath io connection layer in conjunction with
    write/adios_flexpath.c

*/
// system libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/times.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>

// evpath libraries
#include <ffs.h>
#include <atl.h>
//#include <gen_thread.h>
#include <evpath.h>

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

// local libraries
#include "config.h"
#include "public/adios.h"
#include "public/adios_types.h"
#include "public/adios_read_v2.h"
#include "core/adios_read_hooks.h"
#include "core/adios_logger.h"
#include "core/a2sel.h"
#include "public/adios_error.h"
#define  FLEXPATH_SIDE "READER"
#include "core/flexpath.h"
#include "core/futils.h"
#include "core/globals.h"

#include "core/transforms/adios_transforms_common.h" // NCSU ALACRITY-ADIOS

// conditional libraries
#ifdef DMALLOC
#include "dmalloc.h"
#endif

#define FP_BATCH_SIZE 100

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

//This is a linked list for the linked list of fp_vars
//This is necessary to support push based gloabl metadata updates

typedef struct _global_metadata
{
    evgroup * metadata;
    struct _global_metadata * next;
} global_metadata, * global_metadata_ptr;

typedef struct _bridge_info
{
    EVstone bridge_stone;
    EVsource read_source;
    EVsource finalize_source;
    int their_num;
    char *contact;
    int created;
    int opened;
    int step;
    int scheduled;
}bridge_info;

typedef struct _flexpath_read_request
{
    long pad1, pad2, pad3, pad4, pad5;
    int num_pending;
    int num_completed;
    int condition;
}flexpath_read_request;

typedef struct _flexpath_var_chunk
{
    int has_data;
    int rank; // the writer's rank this chunk represents. not used or needed right now
    void *data;
    void *user_buf;
} flexpath_var_chunk;

/*
 * Contains start & counts for each dimension for a writer_rank.
 */
typedef struct _array_displ
{
    int writer_rank;
    int ndims;
    uint64_t pos;
    uint64_t *start;
    uint64_t *count;
}array_displacements;

typedef struct _flexpath_var
{
    int id;
    char *varname;
    char *varpath;

    enum ADIOS_DATATYPES type;
    uint64_t type_size; // type size, not arrays size

    int time_dim; // -1 means it is not a time dimension
    int ndims;
    uint64_t *global_dims; // ndims size (if ndims>0)
    uint64_t *local_dims; // for local arrays NOTE**Isn't currently used...do we have this functionality
    uint64_t array_size; // not relevant for scalars

    int num_chunks;
    flexpath_var_chunk *chunks;

    int num_displ;
    array_displacements *displ;

    ADIOS_SELECTION *sel;
    uint64_t start_position;

    struct _flexpath_var *next;
} flexpath_var;

typedef struct _fp_var_list
{
    int timestep;
    int is_list_filled;
    flexpath_var * var_list;    
    struct _fp_var_list * next;
} timestep_seperated_var_list;

typedef struct _flexpath_reader_file
{
    char *file_name;
    char *group_name; // assuming one group per file right now.
    int host_language;

    int verbose;

    EVstone stone;

    MPI_Comm comm;
    int rank;
    int size;

    int num_bridges;
    bridge_info *bridges;
    int writer_coordinator;
    int writer_coordinator_end;

    int num_vars;
    char ** var_namelist;
    timestep_seperated_var_list * ts_var_list;
    global_metadata_ptr global_info;
    evgroup * current_global_info;
    //timestep_condition_ptr global_metadata_conditions;

    int writer_finalized;
    int last_writer_step;
    int mystep;
    int num_sendees;
    int *sendees;
    read_request_msg *var_read_requests;

    uint64_t data_read; // for perf measurements.
    double time_in; // for perf measurements.
    flexpath_read_request req;
    int inq_var_ready_flag; //This is the variable we will check for setting flexpath_vars
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_condition;
} flexpath_reader_file;

typedef struct _local_read_data
{
    // MPI stuff
    MPI_Comm comm;
    int rank;
    int size;

    // EVPath stuff
    CManager cm;
    atom_t CM_TRANSPORT;
} flexpath_read_data;

flexpath_read_data* fp_read_data = NULL;

/********** Helper functions. **********/

void build_bridge(bridge_info* bridge)
{
    attr_list contact_list = attr_list_from_string(bridge->contact);
    if (bridge->created == 0) {
	bridge->bridge_stone =
	    EVcreate_bridge_action(fp_read_data->cm,
				   contact_list,
				   (EVstone)bridge->their_num);

        bridge->read_source = 
            EVcreate_submit_handle(fp_read_data->cm,
                                    bridge->bridge_stone,
                                    read_request_format_list);

        bridge->finalize_source = 
            EVcreate_submit_handle(fp_read_data->cm,
                                    bridge->bridge_stone,
                                    finalize_close_msg_format_list);

	bridge->created = 1;
    }
}

/* This function returns an array of size = num_readers
   that dictates which writer each reader wants to receive
   information from
*/
static int *
get_writer_array(flexpath_reader_file * fp)
{
    int * the_array = malloc(fp->size * sizeof(int));
    for(int i = 0; i < fp->size; i++)
    {
        if(fp->size < fp->num_bridges)
            the_array[i] = (fp->num_bridges/fp->size) * i;
        else
            the_array[i] = fp->rank % fp->num_bridges;
    }
    fp_verbose(fp, "Finished setting up writer array\n");

    //Do normal reading setup, here.  I wanted to do everything in one place
    //to reduce errors if we have to change things later on.
    if (fp->size < fp->num_bridges) {
    	fp->writer_coordinator = the_array[fp->rank];
    	fp->writer_coordinator_end = (fp->num_bridges/fp->size) * (fp->rank+1);
    	int z;
    	for (z=fp->writer_coordinator; z<fp->writer_coordinator_end; z++) {
    	    build_bridge(&fp->bridges[z]);
    	}
    }
    else {
	int writer_rank = the_array[fp->rank];
	build_bridge(&fp->bridges[writer_rank]);
	fp->writer_coordinator = writer_rank;
        fp->writer_coordinator_end = writer_rank + 1;
    }

    fp_verbose(fp, "Finished setting up our writer_coordinator and building the appropriate bridge\n");

    return the_array;
}


static timestep_seperated_var_list *
find_var_list(flexpath_reader_file * fp, int timestep)
{
    timestep_seperated_var_list * temp = fp->ts_var_list;
    while(temp)
    {
        if(temp->timestep == timestep)
            break;

        temp = temp->next;
    }
    return temp;
}

flexpath_var*
new_flexpath_var(const char *varname, int id, uint64_t type_size)
{
    flexpath_var *var = malloc(sizeof(flexpath_var));
    if (var == NULL) {
	log_error("Error creating new var: %s\n", varname);
	return NULL;
    }

    memset(var, 0, sizeof(flexpath_var));
    // free this when freeing vars.
    var->varname = strdup(varname);
    var->time_dim = -1;
    var->id = id;
    var->type_size = type_size;
    var->displ = NULL;
    return var;
}

//We need to copy the names and set up the flexpath vars when the data comes in
flexpath_var*
pseudo_copy_flexpath_vars(flexpath_var *f)
{
    flexpath_var *vars = NULL;

    while (f != NULL) {
	flexpath_var *curr_var = new_flexpath_var(f->varname, f->id, f->type_size);

	curr_var->num_chunks = 1;
	curr_var->chunks =  malloc(sizeof(flexpath_var_chunk)*curr_var->num_chunks);
	memset(curr_var->chunks, 0, sizeof(flexpath_var_chunk)*curr_var->num_chunks);
	curr_var->sel = NULL;
	curr_var->type = f->type;

        //Make an exact copy of the list if possible
        if(!vars)
            vars = curr_var;
        else
        {
            flexpath_var * last_var = vars;
            while(last_var->next != NULL) last_var = last_var->next;
            last_var->next = curr_var;
        }

	f = f->next;
    }
    return vars;
}

static void
create_flexpath_var_for_timestep(flexpath_reader_file * fp, int timestep)
{
    timestep_seperated_var_list * curr = fp->ts_var_list;

    while(curr && curr->next != NULL)
    {
        if(curr->timestep == timestep)
        {
            fp_verbose(fp, "Already created the timestep for timestep:%d\n", timestep);
            return;
        }
        curr = curr->next;
    }

    if(!curr)
    {
        fp->ts_var_list = calloc(1, sizeof(*fp->ts_var_list));
        fp->ts_var_list->timestep = timestep;
    }
    else
    {
        curr->next = calloc(1, sizeof(*fp->ts_var_list));
        curr->next->timestep = timestep;
        curr->next->var_list = pseudo_copy_flexpath_vars(fp->ts_var_list->var_list);
    } 

}

void
free_displacements(array_displacements *displ, int num)
{
    if (displ) {
	int i;
	for (i=0; i<num; i++) {
	    free(displ[i].start);
	    free(displ[i].count);
	}
	free(displ);
    }
}

static void
flexpath_var_free(flexpath_var * tmpvars)
{
    while (tmpvars) {

        if(tmpvars->varname) {
            free(tmpvars->varname);
            tmpvars->varname = NULL;
        }

        if(tmpvars->varpath) {
            free(tmpvars->varpath);
            tmpvars->varpath = NULL;
        }

	if (tmpvars->ndims > 0) {
	    free(tmpvars->global_dims);
	    tmpvars->ndims = 0;
	}
	if (tmpvars->displ) {
	    free_displacements(tmpvars->displ, tmpvars->num_displ);
	    tmpvars->displ = NULL;
	}

	if (tmpvars->sel) {
	    a2sel_free(tmpvars->sel);
	    tmpvars->sel = NULL;
	}

	tmpvars->sel = NULL;
	for (int i=0; i<tmpvars->num_chunks; i++) {
	    flexpath_var_chunk *chunk = &tmpvars->chunks[i];
	    if (chunk->has_data) {
		free(chunk->data);
		chunk->data = NULL;
		chunk->has_data = 0;
	    }
	    chunk->rank = 0;
	}


        flexpath_var * tmp = tmpvars->next;
        free(tmpvars);
	tmpvars = tmp;
    }
}


//Return the number of elements removed, don't remove if there's only one in the list
static int
cleanup_flexpath_vars(flexpath_reader_file * fp, int timestep)
{
    timestep_seperated_var_list * curr = fp->ts_var_list;
    timestep_seperated_var_list * prev = NULL;

    int number_timesteps_in_list = 0;
    int number_removed = 0;
    while(curr)
    {
        number_timesteps_in_list++;
        curr = curr->next;
    }

    curr = fp->ts_var_list;
    while(number_timesteps_in_list > 1 && curr)
    {
        if(curr->timestep <= timestep)
        {
            flexpath_var_free(curr->var_list);
            if(!prev)
            {
                fp->ts_var_list = curr->next;
                free(curr);
                curr = fp->ts_var_list;
            }
            else
            {
                prev->next = curr->next;
                free(curr);
                curr = prev->next;
            }
            number_removed++;
            number_timesteps_in_list--;
            continue;
        }

        prev = curr;
        curr = curr->next;
    }
            
    return number_removed;

}

void
free_evgroup(evgroup *gp)
{
    EVreturn_event_buffer(fp_read_data->cm, gp);
}

//Return the number of elements removed
static int 
remove_relevant_global_data(flexpath_reader_file * fp, int timestep)
{
    global_metadata_ptr curr = fp->global_info;
    global_metadata_ptr prev = NULL;
    int count = 0;
    while(curr != NULL)
    {
        if(timestep >= curr->metadata->step)
        {
            global_metadata_ptr temp;
            if(!prev)
            {
                fp->global_info = curr->next;
                temp = fp->global_info;
            }
            else
            {
                prev->next = curr->next;
                temp = prev->next;
            }

            free_evgroup(curr->metadata);
            free(curr);
            count++;

            curr = temp;
        }
        else
        {
            prev = curr;
            curr = curr->next;
        }
    }
    return count;

}

static evgroup * 
find_relevant_global_data(flexpath_reader_file * fp)
{
    global_metadata_ptr curr = fp->global_info;
    int reader_step = fp->mystep;
    while(curr)
    {
        if(curr->metadata->step == reader_step)
            break;

        curr = curr->next;
    }

    if(!curr)
    {
        fprintf(stderr, "Error: evgroup queue has been messed up!\n");
        return NULL;
    }

    return curr->metadata;
}

flexpath_var *
find_fp_var(flexpath_var * var_list, const char * varname)
{
    while (var_list) {
	if (!strcmp(varname, var_list->varname)) {
	    return var_list;
	}
	var_list = var_list->next;
    }
    return NULL;
}

static void
share_global_information(flexpath_reader_file * fp)
{
    for (int i = 0; i < fp->current_global_info->num_vars; i++)
    {
        global_var *gblvar = &(fp->current_global_info->vars[i]);
        pthread_mutex_lock(&(fp->queue_mutex));
        timestep_seperated_var_list * ts_var_list = find_var_list(fp, fp->mystep);
        if(!ts_var_list) 
        {
            fprintf(stderr, "Error: could not find var list after it was reported that we had it!!\n");
            if(!1)
            {
                fprintf(stderr, "Severe logic error!\n");
                exit(1);
            }
        }
        flexpath_var *fpvar = find_fp_var(ts_var_list->var_list, gblvar->name);
        pthread_mutex_unlock(&(fp->queue_mutex));

        if (fpvar) {
            offset_struct *offset = &gblvar->offsets[0];
            uint64_t *global_dimensions = offset->global_dimensions;
            fpvar->ndims = offset->offsets_per_rank;
            fpvar->global_dims = malloc(sizeof(uint64_t)*fpvar->ndims);
            memcpy(fpvar->global_dims, global_dimensions, sizeof(uint64_t)*fpvar->ndims);
        } else {
            adios_error(err_corrupted_variable,
        		"Mismatch between global variables and variables specified %s.",
        		gblvar->name);
            fprintf(stderr, "Error: Global variable mismatch!!\n");
            //Not sure what the protocol is here, we will have to figure that out later
            //return err_corrupted_variable;
        }
    }
}

static void
reverse_dims(uint64_t *dims, int len)
{
    int i;
    for (i = 0; i<(len/2); i++) {
        uint64_t tmp = dims[i];
        int end = len-1-i;
        //printf("%d %d\n", dims[i], dims[end]);
        dims[i] = dims[end];
        dims[end] = tmp;
    }
}





flexpath_reader_file*
new_flexpath_reader_file(const char *fname)
{
    flexpath_reader_file *fp = calloc(1, sizeof(*fp));
    if (fp == NULL) {
	log_error("Cannot create data for new file.\n");
	exit(1);
    }
    fp_verbose_init(fp);
    fp->file_name = strdup(fname);
    fp->writer_coordinator = -1;
    fp->last_writer_step = -1;
    fp->req = (flexpath_read_request) {.pad1 = 0, .pad2 = 0, .pad3 = 0, .pad4 = 0, .pad5=0,.num_pending = 0, .num_completed = 0, .condition = -1};
    pthread_mutex_init(&fp->queue_mutex, NULL);
    pthread_cond_init(&fp->queue_condition, NULL);
    return fp;
}

enum ADIOS_DATATYPES
ffs_type_to_adios_type(const char *ffs_type, int size)
{
    char *bracket = "[";
    size_t posfound = strcspn(ffs_type, bracket);
    char *filtered_type = NULL;
    if (strlen(ffs_type) == strlen(bracket)) {
        filtered_type = strdup(ffs_type);
    }
    else {
        filtered_type = malloc(posfound+1);
        memset(filtered_type, '\0', posfound+1);
        filtered_type = strncpy(filtered_type, ffs_type, posfound);
    }

    if (filtered_type[0] == '*') {
	/*  skip "*(" at the beginning */
	filtered_type += 2;
    }
    if (!strcmp("integer", filtered_type)) {
	if (size == sizeof(int)) {
	    return adios_integer;
	} else if (size == sizeof(long)) {
	    return adios_long;
	}
    }
    else if (!strcmp("unsigned integer", filtered_type)) {
	    if (size == sizeof(unsigned int)) {
		return adios_unsigned_integer;
	    } else if (size == sizeof(unsigned long)) {
		return adios_unsigned_long;
	    }
	}	   
    else if (!strcmp("float", filtered_type))
	return adios_real;
    else if (!strcmp("string", filtered_type))
	return adios_string;
    else if (!strcmp("double", filtered_type)) {
	if (size == sizeof(double)) {
	    return adios_double;
	} else if (size == sizeof(long double)) {
	    return adios_long_double;
	}
    }
    else if (!strcmp("char", filtered_type))
	return adios_byte;
    else if (!strcmp("complex", filtered_type))
	return adios_complex;
    else if (!strcmp("double_complex", filtered_type))
        return adios_double_complex;

    fprintf(stderr, "returning unknown for: ffs_type: %s\n", ffs_type);
    return adios_unknown;
}

ADIOS_VARINFO*
convert_var_info(flexpath_var * fpvar,
		 ADIOS_VARINFO * v,
		 const char* varname,
		 const ADIOS_FILE *adiosfile)
{
    int i;
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    v->type = fpvar->type;
    v->ndim = fpvar->ndims;
    // needs to change. Has to get information from write.
    v->nsteps = 1;
    v->nblocks = malloc(sizeof(int)*v->nsteps);
    v->sum_nblocks = 1;
    v->nblocks[0] = 1;
    v->statistics = NULL;
    v->blockinfo = NULL;

    if (v->ndim == 0) {
	int value_size = fpvar->type_size;
	v->value = malloc(value_size);
	if(!v->value) {
	    adios_error(err_no_memory,
			"Cannot allocate buffer in adios_read_flexpath_inq_var()");
	    return NULL;
	}
	flexpath_var_chunk * chunk = &fpvar->chunks[0];
	memcpy(v->value, chunk->data, value_size);
	v->global = 0;
    } else { // arrays
	v->dims = (uint64_t*)malloc(v->ndim * sizeof(uint64_t));
	if (!v->dims) {
	    adios_error(err_no_memory,
			"Cannot allocate buffer in adios_read_flexpath_inq_var()");
	    return NULL;
	}
	// broken.  fix. -- why did I put this comment here?
	int cpysize = fpvar->ndims*sizeof(uint64_t);
	if (fpvar->global_dims) {
	    v->global = 1;
	    memcpy(v->dims, fpvar->global_dims, cpysize);
	}
	else {
	    v->global = 0;
	}
    }
    return v;
}


global_var*
find_gbl_var(global_var *vars, const char *name, int num_vars)
{
    int i;
    for (i=0; i<num_vars; i++) {
	if (!strcmp(vars[i].name, name))
	    return &vars[i];
    }
    return NULL;
}

static FMField*
find_field_by_name (const char *name, const FMFieldList flist)
{
    FMField *f = flist;
    while (f->field_name != NULL)
    {
        if (!strcmp(name, f->field_name))
            return f;
        else
            f++;
    }
    return NULL;
}

static uint64_t
calc_ffspacket_size(FMField *f, attr_list attrs, void *buffer)
{
    uint64_t size = 0;
    while (f->field_name) {
        char atom_name[200] = "";

        strcat(atom_name, FP_NDIMS_ATTR_NAME);
        strcat(atom_name, "_");
        strcat(atom_name, f->field_name);
        int num_dims = 0;
	if (!get_int_attr(attrs, attr_atom_from_string(atom_name), &num_dims)) {
	    fprintf(stderr, "Lookup failure in calc_ffs_packet_size, dims attribute %s\n", atom_name);
	}
	if (num_dims == 0) {
	    size += (uint64_t)f->field_size;
	}
	else {
	    int i;
	    uint64_t tmpsize = (uint64_t)f->field_size;
	    for (i=0; i<num_dims; i++) {
		char *dim;
		char atom_name[200] ="";
		strcat(atom_name, FP_DIM_ATTR_NAME);
		strcat(atom_name, "_");
		strcat(atom_name, f->field_name);
		strcat(atom_name, "_");
		char dim_num[10] = "";
		sprintf(dim_num, "%d", i+1);
		strcat(atom_name, dim_num);
		if (!get_string_attr(attrs, attr_atom_from_string(atom_name), &dim)) {
		    fprintf(stderr, "LOOKUP FAILURE in get_string_attr, %s\n", atom_name);
		}


		FMField *temp_field = find_field_by_name(dim, f);
		uint64_t *temp_val = get_FMfieldAddr_by_name(temp_field,
							     temp_field->field_name,
							     buffer);
		uint64_t dimval = *temp_val;
		tmpsize *= dimval;
	    }
	    size += tmpsize;
	}
	f++;
    }
    return size;
}

void
print_displacement(array_displacements *disp, int myrank)
{
    printf("rank: %d, writer displacements for writer: %d\n", myrank, disp->writer_rank);
    printf("\tndims: %d, pos: %d\n", disp->ndims, (int)disp->pos);

    int i;
    printf("\tstarts: ");
    for (i = 0; i<disp->ndims; i++) {
	printf("%d ", (int)disp->start[i]);
    }
    printf("\n");
    printf("\tcounts: ");
    for (i = 0; i<disp->ndims; i++) {
	printf("%d ", (int)disp->count[i]);
    }
    printf("\n");
}

/*
 * Finds the array displacements for a writer identified by its rank.
 */
array_displacements*
find_displacement(array_displacements *list, int rank, int num_displ)
{
    int i;
    for (i=0; i<num_displ; i++) {
	if (list[i].writer_rank == rank)
	    return &list[i];
    }
    return NULL;
}

array_displacements*
get_writer_displacements(
    int writer_rank,
    const ADIOS_SELECTION * sel,
    global_var* gvar,
    uint64_t *size)
{
    int ndims = sel->u.bb.ndim;
    //where do I free these?
    array_displacements *displ = malloc(sizeof(array_displacements));
    displ->writer_rank = writer_rank;

    displ->start = malloc(sizeof(uint64_t) * ndims);
    displ->count = malloc(sizeof(uint64_t) * ndims);
    memset(displ->start, 0, sizeof(uint64_t) * ndims);
    memset(displ->count, 0, sizeof(uint64_t) * ndims);

    displ->ndims = ndims;
    uint64_t *offsets = gvar->offsets[0].local_offsets;
    uint64_t *local_dims = gvar->offsets[0].local_dimensions;
    uint64_t pos = writer_rank * gvar->offsets[0].offsets_per_rank;

    int i;
    int _size = 1;
    for (i=0; i<ndims; i++) {
	if (sel->u.bb.start[i] >= offsets[pos+i]) {
	    int start = sel->u.bb.start[i] - offsets[pos+i];
	    displ->start[i] = start;
	}


	if ((sel->u.bb.start[i] + sel->u.bb.count[i] - 1) <=
	   (offsets[pos+i] + local_dims[pos+i] - 1)) {
	    int count = ((sel->u.bb.start[i] + sel->u.bb.count[i] - 1) -
			 offsets[pos+i]) - displ->start[i] + 1;
	    displ->count[i] = count;
	} else {
	    int count = (local_dims[pos+i] - 1) - displ->start[i] + 1;
	    displ->count[i] = count;
	}


	_size *= displ->count[i];
    }
    *size = _size;
    return displ;
}

int
need_writer(
    flexpath_reader_file *fp,
    int writer,
    const ADIOS_SELECTION *sel,
    evgroup_ptr gp,
    char *varname)
{
    //select var from group
    global_var *gvar = find_gbl_var(gp->vars, varname, gp->num_vars);

    //for each dimension
    int i=0;
    offset_struct var_offsets = gvar->offsets[0];
    for (i=0; i< var_offsets.offsets_per_rank; i++) {
	int pos = writer*(var_offsets.offsets_per_rank) + i;

        uint64_t sel_offset = sel->u.bb.start[i];
        uint64_t sel_size = sel->u.bb.count[i];

        uint64_t rank_offset = var_offsets.local_offsets[pos];
        uint64_t rank_size = var_offsets.local_dimensions[pos];

	/* fprintf(stderr, "need writer rank: %d writer: %d sel_start: %d sel_count: %d rank_offset: %d rank_size: %d\n",
           fp->rank, writer, (int)sel_offset, (int)sel_size, (int)rank_offset, (int)rank_size); */

        if ((rank_size == 0) || (sel_size == 0)) return 0;

        if (!((rank_offset <= sel_offset+sel_size) && (sel_offset <= rank_offset+rank_size))) {
            /* fprintf(stderr, "Didn't overlap\n"); */
            return 0;
        }

    }
    return 1;
}

void
free_fmstructdesclist(FMStructDescList struct_list)
{
    FMField *f = struct_list[0].field_list;

    //cant free field_name because it's const.
    /* FMField *temp = f; */
    /* while (temp->field_name) { */
    /* 	free(temp->field_name); */
    /* 	temp++; */
    /* } */
    free(f);
    free(struct_list[0].opt_info);
    free(struct_list);
}

int
get_ndims_attr(const char *field_name, attr_list attrs)
{
    char atom_name[200] = "";
    if (strncmp("FPDIM_", field_name, 6) == 0) return 0;
    strcat(atom_name, FP_NDIMS_ATTR_NAME);
    strcat(atom_name, "_");
    strcat(atom_name, field_name);
    int num_dims = 0;
    atom_t atm = attr_atom_from_string(atom_name);
    if (!get_int_attr(attrs, atm, &num_dims)) {
	fprintf(stderr, "LOOKUP FAILURE in get_ndims_attr, %s\n", atom_name);
    }
    return num_dims;
}

flexpath_var*
setup_flexpath_vars(FMField *f, int *num)
{
    flexpath_var *vars = NULL;
    int var_count = 0;

    while (f->field_name != NULL) {
	char *unmangle = flexpath_unmangle(f->field_name);
	flexpath_var *curr_var = new_flexpath_var(unmangle,
						  var_count,
						  f->field_size);
	curr_var->num_chunks = 1;
	curr_var->chunks =  malloc(sizeof(flexpath_var_chunk)*curr_var->num_chunks);
	memset(curr_var->chunks, 0, sizeof(flexpath_var_chunk)*curr_var->num_chunks);
	curr_var->sel = NULL;
	curr_var->type = ffs_type_to_adios_type(f->field_type, f->field_size);
	flexpath_var *temp = vars;
	curr_var->next = temp;
	vars = curr_var;
	if (strncmp(unmangle, "FPDIM", 5)) {
	    var_count++;
	}
	f++;
    }
    *num = var_count;
    return vars;
}


/*****************Messages to writer procs**********************/

void
send_finalize_msg(flexpath_reader_file *fp)
{
    if((fp->rank / fp->num_bridges) == 0)
    {
        for(int i = fp->writer_coordinator; i < fp->writer_coordinator_end; i++)
        {
            finalize_close_msg msg;
            msg.finalize = 1;
            msg.close = 1;
            msg.final_timestep = fp->last_writer_step;
            EVsubmit(fp->bridges[i].finalize_source, &msg, NULL);
        }
    }
    
}

void send_read_msg(flexpath_reader_file *fp, int index, int use_condition)
{
    //Initial sanity check
    if(index >= fp->num_sendees)
    {
        fprintf(stderr, "Error: index requested greater than num_sendees\n");
        exit(1);
    }

    read_request_msg_ptr msg = fp->var_read_requests + index;
    int destination = fp->sendees[index];
    //Give it a condition if we need to block
    //and then set the lamport min
    if(use_condition)
        msg->condition = CMCondition_get(fp_read_data->cm, NULL);
    else
        msg->condition = -1;

    //TODO: Change this part for the EVstore stuff
    msg->current_lamport_min = -1;
    //Basic error checking so we don't break on simple things in the future



    if(!fp->bridges[destination].opened) 
    {
        fprintf(stderr, "Error: trying to send to a bridge that is not open yet!\n");
        exit(1);
    }

    fp_verbose(fp, "Submitting read request to %d\n", destination);
    EVsubmit(fp->bridges[destination].read_source, msg, NULL);
    if(use_condition) {
	fp_verbose(fp, "WAIT in read_request_msg send\n");
	CMCondition_wait(fp_read_data->cm, msg->condition);
	fp_verbose(fp, "Done with WAIT\n");
    }
}

void
add_var_to_read_message(flexpath_reader_file *fp, int destination, char *varname)
{
        int i = 0;
        int found = 0;
        int index = -1;
        for (i=0; i<fp->num_sendees; i++) {
            if (fp->sendees[i]==destination) {
                index = i;
                break;
            }
        }
        if (index == -1) {
            fp->num_sendees+=1;
            fp->sendees=realloc(fp->sendees, fp->num_sendees*sizeof(int));
            fp->var_read_requests=realloc(fp->var_read_requests, fp->num_sendees*sizeof(read_request_msg));
            fp->sendees[fp->num_sendees-1] = destination;
            fp->var_read_requests[fp->num_sendees-1].process_return_id = fp->rank;
            fp->var_read_requests[fp->num_sendees-1].timestep_requested = fp->mystep;
            fp->var_read_requests[fp->num_sendees-1].current_lamport_min = -1;
            fp->var_read_requests[fp->num_sendees-1].condition = -1;
            fp->var_read_requests[fp->num_sendees-1].var_name_array = malloc(sizeof(char*));
            fp->var_read_requests[fp->num_sendees-1].var_name_array[0] = strdup(varname);
            fp->var_read_requests[fp->num_sendees-1].var_count = 1;
        } else {
            read_request_msg_ptr msg = &(fp->var_read_requests[index]);
            msg->var_name_array = realloc(msg->var_name_array, sizeof(char*)*(msg->var_count +1));
            msg->var_name_array[msg->var_count] = strdup(varname);
            msg->var_count++;
        }
        if (!fp->bridges[destination].created) {
            build_bridge(&(fp->bridges[destination]));
	}
        //TODO: Remove the open nature here
	if (!fp->bridges[destination].opened) {
	    fp->bridges[destination].opened = 1;
	}
}

/********** EVPath Handlers **********/

/*
static int
update_step_msg_handler(
    CManager cm,
    void *vevent,
    void *client_data,
    attr_list attrs)
{
    update_step_msg *msg = (update_step_msg*)vevent;
    ADIOS_FILE *adiosfile = (ADIOS_FILE*)client_data;
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;

    fp->last_writer_step = msg->step;
    fp->writer_finalized = msg->finalized;
    adiosfile->last_step = msg->step;
    CMCondition_signal(fp_read_data->cm, msg->condition);
    return 0;
}
*/

static int
group_msg_handler(CManager cm, void *vevent, void *client_data, attr_list attrs)
{
    EVtake_event_buffer(fp_read_data->cm, vevent);
    evgroup *msg = (evgroup*)vevent;
    ADIOS_FILE *adiosfile = client_data;
    flexpath_reader_file * fp = (flexpath_reader_file*)adiosfile->fh;
    if (fp->group_name == NULL) {
        fp->group_name = strdup(msg->group_name);
    }
    
    pthread_mutex_lock(&(fp->queue_mutex));
    global_metadata_ptr curr = fp->global_info;
    while(curr && curr->next)
        curr = curr->next;

    if(!curr)
    {
        fp->global_info = calloc(1, sizeof(global_metadata));
        curr = fp->global_info;
    }
    else
    {
        curr->next = calloc(1, sizeof(global_metadata));
        curr = curr->next;
    }

    curr->metadata = msg;
    pthread_mutex_unlock(&(fp->queue_mutex));
    return 0;
}

static int
finalize_msg_handler(CManager cm, void * vevent, void * client_data, attr_list attrs)
{
    ADIOS_FILE *adiosfile = client_data;
    flexpath_reader_file * fp = (flexpath_reader_file*)adiosfile->fh;
    finalize_close_msg_ptr msg = (finalize_close_msg_ptr)vevent;
    fp_verbose(fp, "Received the finalize message from the writer\n");
    pthread_mutex_lock(&(fp->queue_mutex));
    fp->writer_finalized = 1;
    fp->last_writer_step = msg->final_timestep;
    pthread_cond_signal(&(fp->queue_condition));
    pthread_mutex_unlock(&(fp->queue_mutex));
    return 0;
}

void
map_local_to_global_index(uint64_t ndim, uint64_t *local_index, uint64_t *local_offsets, uint64_t *global_index)
{
    int i;
    for (i=0; i < ndim; i++) {
	global_index[i] = local_index[i] + local_offsets[i];
    }
}

void
map_global_to_local_index(uint64_t ndim, uint64_t *global_index, uint64_t *local_offsets, uint64_t *local_index)
{
    int i;
    for (i=0; i < ndim; i++) {
	local_index[i] = global_index[i] - local_offsets[i];
    }
}

int
find_offset(uint64_t ndim, uint64_t *size, uint64_t *index)
{
    int offset = 0;
    int i;
    for (i=0; i< ndim; i++) {
	offset = index[i] + (size[i] * offset);
    }
    return offset;
}

/*
 *  element_size is the byte size of the array elements
 *  dims is the number of dimensions in the variable
 *  global_dimens is an array, dims long, giving the size of each dimension
 *  partial_offsets is an array, dims long, giving the starting offsets per dimension
 *      of this data block in the global array
 *  partial_counts is an array, dims long, giving the size per dimension
 *      of this data block in the global array
 *  selection_offsets is an array, dims long, giving the starting offsets in the global array
 *      of the output selection.
 *  selection_counts is an array, dims long, giving the size per dimension
 *      of the output selection.
 *  data is the input, a slab of the global array
 *  selection is the output, to be filled with the selection array.
 */
void
extract_selection_from_partial(int element_size, uint64_t dims, uint64_t *global_dimens,
			       uint64_t *partial_offsets, uint64_t *partial_counts,
			       uint64_t *selection_offsets, uint64_t *selection_counts,
			       char *data, char *selection)
{
    int block_size;
    int source_block_stride;
    int dest_block_stride;
    int source_block_start_offset;
    int dest_block_start_offset;
    int block_count;
    int dim;
    int operant_dims;
    int operant_element_size;

    block_size = 1;
    operant_dims = dims;
    operant_element_size = element_size;
    for (dim = dims-1; dim >= 0; dim--) {
	if ((global_dimens[dim] == partial_counts[dim]) &&
	    (selection_counts[dim] == partial_counts[dim])) {
	    block_size *= global_dimens[dim];
	    operant_dims--;   /* last dimension doesn't matter, we got all and we want all */
	    operant_element_size *= global_dimens[dim];
	} else {
	    int left = MAX(partial_offsets[dim], selection_offsets[dim]);
	    int right = MIN(partial_offsets[dim] + partial_counts[dim],
			    selection_offsets[dim] + selection_counts[dim]);
	    block_size *= (right - left);
	    break;
	}
    }
    source_block_stride = partial_counts[operant_dims-1] * operant_element_size;
    dest_block_stride = selection_counts[operant_dims-1] * operant_element_size;

    /* calculate first selected element and count */
    block_count = 1;
    uint64_t *first_index = malloc(dims * sizeof(first_index[0]));
    for (dim = 0; dim < dims; dim++) {
	int left = MAX(partial_offsets[dim], selection_offsets[dim]);
	int right = MIN(partial_offsets[dim] + partial_counts[dim],
			selection_offsets[dim] + selection_counts[dim]);
	if (dim < operant_dims-1) {
	    block_count *= (right-left);
	}
	first_index[dim] = left;
    }
    uint64_t *selection_index = malloc(dims * sizeof(selection_index[0]));
    map_global_to_local_index(dims, first_index, selection_offsets, selection_index);
    dest_block_start_offset = find_offset(dims, selection_counts, selection_index);
    free(selection_index);
    dest_block_start_offset *= element_size;

    uint64_t *partial_index = malloc(dims * sizeof(selection_index[0]));
    map_global_to_local_index(dims, first_index, partial_offsets, partial_index);
    source_block_start_offset = find_offset(dims, partial_counts, partial_index);
    free(partial_index);
    source_block_start_offset *= element_size;

    data += source_block_start_offset;
    selection += dest_block_start_offset;
    int i;
    for (i=0 ; i < block_count; i++) {
	memcpy(selection, data, block_size * element_size);
	data += source_block_stride;
	selection += dest_block_stride;
    }
}

    static int
raw_handler(CManager cm, void *vevent, int len, void *client_data, attr_list attrs)
{
    ADIOS_FILE *adiosfile = client_data;
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    fp_verbose(fp, "Received data message!\n");
    int writer_rank;
    int timestep;
    int only_scalars = 0;
    get_int_attr(attrs, attr_atom_from_string(FP_RANK_ATTR_NAME), &writer_rank);
    get_int_attr(attrs, attr_atom_from_string(FP_TIMESTEP), &timestep);
    get_int_attr(attrs, attr_atom_from_string(FP_ONLY_SCALARS), &only_scalars);
    /* fprintf(stderr, "\treader rank:%d:got data from writer:%d:step:%d\n", */
    /* 	    fp->rank, writer_rank, fp->mystep); */
    FMContext context = CMget_FMcontext(cm);
    void *base_data = FMheader_skip(context, vevent);
    FMFormat format = FMformat_from_ID(context, vevent);

    // copy //FMfree_struct_desc_list call
    FMStructDescList struct_list =
	FMcopy_struct_list(format_list_of_FMFormat(format));
    FMField *f = struct_list[0].field_list;
#if 0
    uint64_t packet_size = calc_ffspacket_size(f, attrs, base_data);
    fp->data_read += packet_size;
#endif
    /* setting up initial vars from the format list that comes along with the
       message. Message contains both an FFS description and the data. */
    if (fp->num_vars == 0) {
        fp_verbose(fp, "Setting up initial vars!\n");
	int var_count = 0;

        pthread_mutex_lock(&(fp->queue_mutex));
        create_flexpath_var_for_timestep(fp, timestep);
        timestep_seperated_var_list * ts_var_list = find_var_list(fp, timestep);
	ts_var_list->var_list = setup_flexpath_vars(f, &var_count);
        pthread_mutex_unlock(&(fp->queue_mutex));

	adiosfile->var_namelist = malloc(var_count * sizeof(char *));
	int i = 0;
	while (f->field_name != NULL) {
	    char *unmangle = flexpath_unmangle(f->field_name);
	    if (strncmp(unmangle, "FPDIM", 5)) {
		adiosfile->var_namelist[i++] = unmangle;
	    } else {
		free(unmangle);
	    }
	    f++;
	}
	adiosfile->nvars = var_count;
	fp->num_vars = var_count;
    }

    f = struct_list[0].field_list;
    char *curr_offset = NULL;

    while (f->field_name) {
        char atom_name[200] = "";
	char *unmangle = flexpath_unmangle(f->field_name);
	char *dims[100]; /* more than we should ever need */

        pthread_mutex_lock(&(fp->queue_mutex));
        timestep_seperated_var_list * ts_var_list = find_var_list(fp, timestep);
        if(ts_var_list == NULL)
        {
            fp_verbose(fp, "Created timestep in raw handler -- timestep:%d\n", timestep);
            create_flexpath_var_for_timestep(fp, timestep);
            ts_var_list = find_var_list(fp, timestep);
        }
	flexpath_var * var = find_fp_var(ts_var_list->var_list, unmangle);
        pthread_mutex_unlock(&(fp->queue_mutex));

    	if (!var) {
    	    adios_error(err_file_open_error,
    			"file not opened correctly.  var does not match format.\n");
    	    return err_file_open_error;
    	}

	int num_dims = 0;
	if (f->field_type) {
	    char *tmp = index(f->field_type, '[');
	    while (tmp != NULL) {
		int len;
		tmp++;
		len = index(tmp, ']') - tmp;
		dims[num_dims] = calloc(len + 1, 1);
		strncpy(dims[num_dims], tmp, len);
		tmp = index(tmp+1, '[');
		num_dims++;
	    }
	}
    	var->ndims = num_dims;
	flexpath_var_chunk *curr_chunk = &var->chunks[0];

	// Has the var been scheduled
	if (var->sel) {
            fp_verbose(fp, "Vars have been scheduled for timestep:%d\n", timestep);
	    if (var->sel->type == ADIOS_SELECTION_WRITEBLOCK) {
		if (num_dims == 0) { // writeblock selection for scalar
		    if (var->sel->u.block.index == writer_rank) {
			void *tmp_data = get_FMfieldAddr_by_name(f,
								 f->field_name,
								 base_data);
			memcpy(var->chunks[0].user_buf, tmp_data, f->field_size);
		    }
		}
		else { // writeblock selection for arrays
		    if (var->sel->u.block.index == writer_rank) {
			var->array_size = var->type_size;
			int i;
			for (i=0; i<num_dims; i++) {
			    char *dim = dims[i];
			    FMField *temp_field = find_field_by_name(dim, struct_list[0].field_list);
			    if (!temp_field) {
				adios_error(err_corrupted_variable,
					    "Could not find fieldname: %s\n",
					    dim);
			    } else {
				int *temp_data = get_FMfieldAddr_by_name
				    (
					temp_field,
					temp_field->field_name,
					base_data
				    );

				uint64_t dim = (uint64_t)(*temp_data);
				var->array_size = var->array_size * dim;
			    }
			    free(dims[i]);
			}
			void *arrays_data  = get_FMPtrField_by_name(f,
								    f->field_name,
								    base_data,
								    1);
			memcpy(var->chunks[0].user_buf, arrays_data, var->array_size);
		    }
		}
	    }
	    else if (var->sel->type == ADIOS_SELECTION_BOUNDINGBOX) {

		if (var->ndims > 0) { // arrays
		    int i;
		    global_var *gv = find_gbl_var(fp->current_global_info->vars,
						  var->varname,
						  fp->current_global_info->num_vars);

		    array_displacements *disp = find_displacement(var->displ,
								  writer_rank,
								  var->num_displ);
		    if (disp) { // does this writer hold a chunk we've asked for

			//print_displacement(disp, fp->rank);

			uint64_t *global_sel_start = var->sel->u.bb.start;
			uint64_t *global_sel_count = var->sel->u.bb.count;
			uint64_t *temp = gv->offsets[0].local_dimensions;
			uint64_t *temp2 = gv->offsets[0].local_offsets;
			uint64_t *global_dimensions = gv->offsets[0].global_dimensions;
			int offsets_per_rank = gv->offsets[0].offsets_per_rank;
			uint64_t *writer_sizes = &temp[offsets_per_rank * writer_rank];
			uint64_t *writer_offsets = &temp2[offsets_per_rank * writer_rank];

			char *writer_array = (char*)get_FMPtrField_by_name(f,
									   f->field_name,
									   base_data, 1);
			char *reader_array = (char*)var->chunks[0].user_buf;

			extract_selection_from_partial(f->field_size, disp->ndims, global_dimensions,
						       writer_offsets, writer_sizes,
						       global_sel_start, global_sel_count,
						       writer_array, reader_array);
		    }
		}
	    }
	}
	if (num_dims == 0) { // only worry about scalars
	    flexpath_var_chunk *chunk = &var->chunks[0];
	    if (!chunk->has_data) {
		void *tmp_data = get_FMfieldAddr_by_name(f, f->field_name, base_data);
		chunk->data = malloc(f->field_size);
		memcpy(chunk->data, tmp_data, f->field_size);
		chunk->has_data = 1;
	    }
	}
        f++;
    }

    //We need this to differentiate between real data messages and the global metadata hack message that gets sent with every
    //EVgroup message on the writer side
    if(!only_scalars)
    {
        fp_verbose(fp, "Reporting received data for timestep:%d\n", timestep);
        fp->req.num_completed++;
        /* fprintf(stderr, "\t\treader rank:%d:step:%d:num_completed:%d:num_pending:%d\n", */
        /* 	    fp->rank, fp->mystep, fp->req.num_completed, fp->req.num_pending); */
        if (fp->req.num_completed == fp->req.num_pending) {
            /* fprintf(stderr, "\t\treader rank:%d:step:%d:signalling_on:%d\n", */
            /* 	fp->rank, fp->mystep, fp->req.condition); */
            CMCondition_signal(fp_read_data->cm, fp->req.condition);
        }

    }
    else
    {
        fp_verbose(fp, "Only scalars message received for:%d\n", timestep);
        pthread_mutex_lock(&(fp->queue_mutex));
        timestep_seperated_var_list * ts_var_list = find_var_list(fp, timestep);
        ts_var_list->is_list_filled = 1;
        pthread_cond_signal(&(fp->queue_condition));
        pthread_mutex_unlock(&(fp->queue_mutex));
    }

    free_fmstructdesclist(struct_list);
    return 0;
}

/********** Core ADIOS Read functions. **********/

extern void
reader_go_handler(CManager cm, CMConnection conn, void *vmsg, void *client_data, attr_list attrs)
{
    reader_go_msg *msg = (reader_go_msg *)vmsg;
    flexpath_reader_file* fp = (flexpath_reader_file *)msg->reader_file;
    CMCondition_signal(cm, fp->req.condition);
}

/*
 * Gathers basic MPI information; sets up reader CM.
 */
int
adios_read_flexpath_init_method (MPI_Comm comm, PairStruct* params)
{
    setenv("CMSelfFormats", "1", 1);
    fp_read_data = malloc(sizeof(flexpath_read_data));
    if (!fp_read_data) {
        adios_error(err_no_memory, "Cannot allocate memory for flexpath.");
        return -1;
    }
    memset(fp_read_data, 0, sizeof(flexpath_read_data));

    fp_read_data->CM_TRANSPORT = attr_atom_from_string("CM_TRANSPORT");
    attr_list listen_list = NULL;
    char * transport = NULL;
    transport = getenv("CMTransport");

    // setup MPI stuffs
    fp_read_data->comm = comm;
    MPI_Comm_size(fp_read_data->comm, &(fp_read_data->size));
    MPI_Comm_rank(fp_read_data->comm, &(fp_read_data->rank));

    fp_read_data->cm = CManager_create();
    if (transport == NULL) {
      int listened = 0;
      while (listened == 0) {
	  if (CMlisten(fp_read_data->cm) == 0) {
	      fprintf(stderr, "Flexpath ERROR: reader %d:pid:%d unable to initialize connection manager. Trying again.\n",
		      fp_read_data->rank, (int)getpid());
	  } else {
	      listened = 1;
	  }
      }
    } else {
	listen_list = create_attr_list();
	add_attr(listen_list, fp_read_data->CM_TRANSPORT, Attr_String,
		 (attr_value)strdup(transport));
	CMlisten_specific(fp_read_data->cm, listen_list);
    }
    int forked = CMfork_comm_thread(fp_read_data->cm);
    if (!forked) {
	fprintf(stderr, "reader %d failed to fork comm_thread.\n", fp_read_data->rank);
	/*log_debug( "forked\n");*/
    }
    CMFormat format = CMregister_simple_format(fp_read_data->cm, "Flexpath reader go", reader_go_field_list, sizeof(reader_go_msg));
    CMregister_handler(format, reader_go_handler, NULL);
    return 0;
}

ADIOS_FILE*
adios_read_flexpath_open_file(const char * fname, MPI_Comm comm)
{
    adios_error (err_operation_not_supported,
                 "FLEXPATH staging method does not support file mode for reading. "
                 "Use adios_read_open() to open a staged dataset.\n");
    return NULL;
}

char *
get_writer_contact_info_filesystem(const char *fname, flexpath_reader_file * fp)
{
    char writer_info_filename[200];

    sprintf(writer_info_filename, "%s_%s", fname, WRITER_CONTACT_FILE);
    FILE *fp_in;
redo:
    fp_in = fopen(writer_info_filename, "r");
    while (!fp_in) {
        //CMsleep(fp_read_data->cm, 1);
        fp_in = fopen(writer_info_filename, "r");
    }
    struct stat buf;
    fstat(fileno(fp_in), &buf);
    int size = buf.st_size;
    if(size == 0)
    {
        fp_verbose(fp, "Size of writer contact file is zero, but it shouldn't be! Retrying!\n");
        goto redo;
    }
    //printf("Size: %d\n", size);
    
    char *buffer = malloc(size);
    int temp = fread(buffer, size, 1, fp_in);
    fclose(fp_in);
    return buffer;
}

/*
 * Still have work to do here.
 * Change it so that we can support the timeouts and lock_modes.
 */
/*
 * Sets up local data structure for series of reads on an adios file
 * - create evpath graph and structures
 * -- create evpath control stone (outgoing)
 * -- create evpath data stone (incoming)
 * -- rank 0 dumps contact info to file
 * -- create connections using contact info from file
 */
ADIOS_FILE*
adios_read_flexpath_open(const char * fname,
			 MPI_Comm comm,
			 enum ADIOS_LOCKMODE lock_mode,
			 float timeout_sec)
{
    ADIOS_FILE *adiosfile = malloc(sizeof(ADIOS_FILE));
    if (!adiosfile) {
	adios_error (err_no_memory,
		     "Cannot allocate memory for file info.\n");
	return NULL;
    }

    flexpath_reader_file *fp = new_flexpath_reader_file(fname);
    fp_verbose(fp, "entering flexpath_open\n");
    fp->host_language = futils_is_called_from_fortran();
    adios_errno = 0;
    fp->stone = EValloc_stone(fp_read_data->cm);
    fp->comm = comm;
    
    adiosfile->fh = (uint64_t)fp;
    adiosfile->current_step = 0;

    MPI_Comm_size(fp->comm, &(fp->size));
    MPI_Comm_rank(fp->comm, &(fp->rank));

    EVassoc_terminal_action(fp_read_data->cm,
			    fp->stone,
			    evgroup_format_list,
			    group_msg_handler,
			    adiosfile);

    EVassoc_terminal_action(fp_read_data->cm,
                            fp->stone,
                            finalize_close_msg_format_list,
                            finalize_msg_handler,
                            adiosfile);
                            
    EVassoc_raw_terminal_action(fp_read_data->cm,
				fp->stone,
				raw_handler,
				adiosfile);

    char *string_list;
    char data_contact_info[CONTACT_LENGTH];
    string_list = attr_list_to_string(CMget_contact_list(fp_read_data->cm));
    sprintf(&data_contact_info[0], "%d:%s", fp->stone, string_list);
    free(string_list);

    //volatile int qur = 0;
    //while(qur == 0) { /*Change qur in debugger */ }
    //MPI_Barrier(MPI_COMM_WORLD);
    char * recvbuf;
    if (fp->rank == 0) {
        char *contact_info;
	recvbuf = (char*)malloc(sizeof(char)*CONTACT_LENGTH*(fp->size));
        fp_verbose(fp, "Running MPI_Gather for reader contact information!\n");
        MPI_Gather(data_contact_info, CONTACT_LENGTH, MPI_CHAR, recvbuf,
                   CONTACT_LENGTH, MPI_CHAR, 0, fp->comm);

        contact_info = get_writer_contact_info_filesystem(fname, fp);
        
        char in_contact[CONTACT_LENGTH] = "";
        int num_bridges = 0;
        int their_stone;
        int return_condition;

        char *send_buffer = malloc(CONTACT_LENGTH);
        void *writer_filedata;
        char *point = contact_info;
        //fprintf(stderr, "%s", point);
        sscanf(point, "%d\n", &return_condition);
        point = index(point, '\n') + 1;
        sscanf(point, "%p\n", &writer_filedata);
        point = index(point, '\n') + 1;
        while (point) {
            sscanf(point, "%d:%[^\t\n]", &their_stone, in_contact);
            point = index(point, '\n'); if (point) point++;
            fp->bridges = realloc(fp->bridges,
                                  sizeof(bridge_info) * (num_bridges+1));
            send_buffer = realloc(send_buffer, (num_bridges+1)*CONTACT_LENGTH);
            fp->bridges[num_bridges].their_num = their_stone;
            fp->bridges[num_bridges].contact = strdup(in_contact);
            sprintf(&send_buffer[num_bridges*CONTACT_LENGTH], "%d:%s", their_stone, in_contact);
            fp->bridges[num_bridges].created = 0;
            fp->bridges[num_bridges].step = 0;
            fp->bridges[num_bridges].opened = 0;
            fp->bridges[num_bridges].scheduled = 0;
            num_bridges++;
        }
        fp->num_bridges = num_bridges;

        // broadcast writer contact info to all reader ranks
        fp_verbose(fp, "Broadcasting writer data to all ranks!\n");
        MPI_Bcast(&fp->num_bridges, 1, MPI_INT, 0, MPI_COMM_WORLD);
        
        MPI_Bcast(send_buffer, fp->num_bridges*CONTACT_LENGTH, MPI_CHAR, 0, MPI_COMM_WORLD);
        
	// prepare to send all ranks contact info to writer root
        reader_register_msg reader_register;
        reader_register.condition = return_condition;
        reader_register.writer_file = (uint64_t) writer_filedata;
        reader_register.reader_file = (uint64_t) fp;
        reader_register.contact_count = fp->size;
        reader_register.contacts = malloc(fp->size * sizeof(char*));
	for (int i=0; i<fp->size; i++) {
            reader_register.contacts[i] = &recvbuf[i*CONTACT_LENGTH];
	}
        int * write_array = get_writer_array(fp);
        reader_register.writer_array = write_array;

        CMFormat format = CMregister_simple_format(fp_read_data->cm, "Flexpath reader register", reader_register_field_list, sizeof(reader_register_msg));
        attr_list writer_rank0_contact = attr_list_from_string(fp->bridges[0].contact);
        CMConnection conn = CMget_conn (fp_read_data->cm, writer_rank0_contact);
        CMwrite(conn, format, &reader_register);
	free(recvbuf);
        free(write_array);
        fp->req.condition = CMCondition_get(fp_read_data->cm, conn);
        /*  loosing connection here.  Close it later */

        /* wait for "go" from writer */
        fp_verbose(fp, "waiting for go message in read_open, WAITING, condition %d\n", fp->req.condition);
        CMCondition_wait(fp_read_data->cm, fp->req.condition);
        fp_verbose(fp, "finished wait for go message in read_open\n");
        MPI_Barrier(MPI_COMM_WORLD);
    } else {
        /* not rank 0 */
        fp_verbose(fp, "About to run the normal setup for bridges before MPI_Gather operation!\n");
        char *this_side_contact_buffer;
        MPI_Gather(data_contact_info, CONTACT_LENGTH, MPI_CHAR, recvbuf,
                   CONTACT_LENGTH, MPI_CHAR, 0, fp->comm);
        MPI_Bcast(&fp->num_bridges, 1, MPI_INT, 0, MPI_COMM_WORLD);
        this_side_contact_buffer = malloc(fp->num_bridges*CONTACT_LENGTH);
        MPI_Bcast(this_side_contact_buffer, fp->num_bridges*CONTACT_LENGTH, MPI_CHAR, 0, MPI_COMM_WORLD);
        fp->bridges = malloc(sizeof(bridge_info) * fp->num_bridges);
        for (int i = 0; i < fp->num_bridges; i++) {
            int their_stone;
            char in_contact[CONTACT_LENGTH];
            sscanf(&this_side_contact_buffer[i*CONTACT_LENGTH], "%d:%s", &their_stone, in_contact);
            fp->bridges[i].their_num = their_stone;
            fp->bridges[i].contact = strdup(in_contact);
            fp->bridges[i].created = 0;
            fp->bridges[i].step = 0;
            fp->bridges[i].opened = 0;
            fp->bridges[i].scheduled = 0;
        }
        fp_verbose(fp, "About to get writer_array after creating the bridges!\n");
        //We need the array for rank 0, but not here. I'm trying really hard to 
        //have the logic determing the reader_writer coordination logic in a single
        //place so we don't have issues when/if we change it later
        int * temp_write = get_writer_array(fp);
        free(temp_write);
        MPI_Barrier(MPI_COMM_WORLD);
        fp_verbose(fp, "Past the MPI_Barrier on the non-root side\n");
    }


    //EVstore Setup


    fp_verbose(fp, "About to lock mutex and access timstep_seperated_var_list\n");
    // requesting initial data.
    pthread_mutex_lock(&(fp->queue_mutex));
    timestep_seperated_var_list * ts_var_list = find_var_list(fp, fp->mystep);
    if(ts_var_list == NULL)
    {
        create_flexpath_var_for_timestep(fp, fp->mystep);
        ts_var_list = find_var_list(fp, fp->mystep);
    }


    while(ts_var_list->is_list_filled == 0) //The scalar data and evgroup message haven't come in yet
    {
        fp_verbose(fp, "Waiting for writer to send the global data from the first timestep\n");
        pthread_cond_wait(&(fp->queue_condition), &(fp->queue_mutex));
    }

    fp->current_global_info = find_relevant_global_data(fp);
    pthread_mutex_unlock(&(fp->queue_mutex));

    
    //Fix the last of the info the reader will need
    share_global_information(fp);
    fp_verbose(fp, "Reader now has all of the information to begin scheduling reads for the first timestep\n");
    
    fp->data_read = 0;
    fp->req.condition = CMCondition_get(fp_read_data->cm, NULL);
    fp->req.num_pending = 1;
    fp->req.num_completed = 0;


    fp->data_read = 0;
    // this has to change. Writer needs to have some way of
    // taking the attributes out of the xml document
    // and sending them over ffs encoded. Not yet implemented.
    // the rest of this info for adiosfile gets filled in raw_handler.
    adiosfile->nattrs = 0;
    adiosfile->attr_namelist = NULL;
    // first step is at least one, otherwise raw_handler will not execute.
    // in reality, writer might be further along, so we might have to make
    // the writer explitly send across messages each time it calls close, to
    // indicate which timesteps are available.
    adiosfile->last_step = 1;
    adiosfile->path = strdup(fname);
    // verifies these two fields. It's not BP, so no BP version.
    // It's a stream, so how can the file size be known?
    adiosfile->version = -1;
    adiosfile->file_size = 0;
    adios_errno = err_no_error;
    fp_verbose(fp, "leaving flexpath_open\n");
    return adiosfile;
}

int adios_read_flexpath_finalize_method ()
{
    /* oddly, ADIOS doesn't let us close a single file.  Instead, this finalizes all operations.  Commented out below is the body that we should have when we can close a file. */

    /* flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh; */
    /* int i; */
    /* for (i=0; i<fp->num_bridges; i++) { */
    /*     if (fp->bridges[i].created && !fp->bridges[i].opened) { */
    /* 	    fp_verbose(fp, "sending open msg in flexpath_release_step\n"); */
    /* 	    send_finalize_msg(fp, i); */
    /*     } */
    /* } */
    return 1;
}


//A lot of the complexity of the underlying linked lists is hidden in the functions
void adios_read_flexpath_release_step(ADIOS_FILE *adiosfile) {
    int i;
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    //fp_verbose(fp, "waiting at flexpath_release step barrier\n");
    //MPI_Barrier(fp->comm);
    //fp_verbose(fp, "done with flexpath_release step barrier\n");

    pthread_mutex_lock(&(fp->queue_mutex));
    remove_relevant_global_data(fp, fp->mystep);
    pthread_mutex_unlock(&(fp->queue_mutex));
    fp->current_global_info = NULL;

    pthread_mutex_lock(&(fp->queue_mutex));
    cleanup_flexpath_vars(fp, fp->mystep);
    pthread_mutex_unlock(&(fp->queue_mutex));
}

int
adios_read_flexpath_advance_step(ADIOS_FILE *adiosfile, int last, float timeout_sec)
{    
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    int count = 0;

    //Check to see if we have the next steps global metadata
    pthread_mutex_lock(&(fp->queue_mutex));
    timestep_seperated_var_list * ts_var_list = find_var_list(fp, ++fp->mystep);
    if(ts_var_list == NULL)
    {
        create_flexpath_var_for_timestep(fp, fp->mystep);
        ts_var_list = find_var_list(fp, fp->mystep);
    }

    //If we don't have the scalar data or if we haven't received a finalized message, wait
    while(ts_var_list->is_list_filled == 0 && (fp->last_writer_step != fp->mystep)) 
    {
        fp_verbose(fp, "Waiting for writer to send the global data for timestep: %d\n", fp->mystep);
        pthread_cond_wait(&(fp->queue_condition), &(fp->queue_mutex));
        fp_verbose(fp, "Received signal! Last_writer_step:%d\t\tMystep:%d\n", fp->last_writer_step, fp->mystep);
    }
    pthread_mutex_unlock(&(fp->queue_mutex));
    fp_verbose(fp, "Finished wait on global data for timestep: %d\n", fp->mystep);

    //If finalized, err_end_of_stream
    if(fp->last_writer_step == fp->mystep)
    {
        fp_verbose(fp, "Received the writer_finalized message! Reader returning err_end_of_stream!\n");
        adios_errno = err_end_of_stream;
        return err_end_of_stream;
    }

    //Remove obsolete bookeeping information and update current_global_info
    pthread_mutex_lock(&(fp->queue_mutex));
    
    int global_data_cleaned = remove_relevant_global_data(fp, fp->mystep - 1);
    int flexpath_var_cleaned = cleanup_flexpath_vars(fp, fp->mystep - 1);
    fp->current_global_info = find_relevant_global_data(fp);
    if(!fp->current_global_info)
    {
        fprintf(stderr, "Severe logic error!\n");
        exit(1);
    }
    fp_verbose(fp, "Cleaned the global data and the flexpath vars!\n"
                    "Global metadata removed: %d\t\tFlexpath vars removed: %d\n", global_data_cleaned, flexpath_var_cleaned);
    pthread_mutex_unlock(&(fp->queue_mutex));

    //Fix the last of the info the reader will need, this locks and unlocks the mutex so that's why we have it out here
    share_global_information(fp);

    //Wait for all readers to reach this step, this should change in extended version
    MPI_Barrier(fp->comm);

    return 0;
}

int adios_read_flexpath_close(ADIOS_FILE * fp)
{
    flexpath_reader_file *file = (flexpath_reader_file*)fp->fh;
    //TODO: Send close message to each opened link


    /*
    start to cleanup.  Clean up var_lists for now, as the
    data has already been copied over to ADIOS_VARINFO structs
    that the user maintains a copy of.
    */
    send_finalize_msg(file);
    
    pthread_mutex_lock(&(file->queue_mutex));
    timestep_seperated_var_list * curr = file->ts_var_list;
    while(curr)
    {
        flexpath_var * v = curr->var_list;
        while (v) {
        	// free chunks; data has already been copied to user
        	int i;
        	for (i = 0; i<v->num_chunks; i++) {
        	    flexpath_var_chunk *c = &v->chunks[i];
    	    if (!c)
    		log_error("FLEXPATH: %s This should not happen! line %d\n",__func__,__LINE__);
    	    //free(c->data);
    	    c->data = NULL;
    	    free(c);
    	}
    	flexpath_var *tmp = v->next;
    	free(v);
    	v = tmp;
        	//v=v->next;
        }
        timestep_seperated_var_list * temp = curr->next;
        free(curr);
        curr = temp;
    }
    file->ts_var_list = NULL;
    pthread_mutex_unlock(&(file->queue_mutex));
    return 0;
}

ADIOS_FILE *adios_read_flexpath_fopen(const char *fname, MPI_Comm comm) {
   return 0;
}

int adios_read_flexpath_is_var_timed(const ADIOS_FILE* fp, int varid) { return 0; }

void adios_read_flexpath_get_groupinfo(
    const ADIOS_FILE *adiosfile,
    int *ngroups,
    char ***group_namelist,
    uint32_t **nvars_per_group,
    uint32_t **nattrs_per_group)
{
    flexpath_reader_file *fp;
    if (adiosfile) {
        fp = (flexpath_reader_file *) adiosfile->fh;
        *ngroups = 1;
        *group_namelist = (char **) malloc (sizeof (char*));
        *group_namelist[0] = strdup (fp->group_name);
    }

}

int adios_read_flexpath_check_reads(const ADIOS_FILE* fp, ADIOS_VARCHUNK** chunk) { log_debug( "flexpath:adios function check reads\n"); return 0; }

int adios_read_flexpath_perform_reads(const ADIOS_FILE *adiosfile, int blocking)
{
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    fp_verbose(fp, "entering perform_reads.\n");
    fp->data_read = 0;
    int i;
    int batchcount = 0;
    int num_sendees = fp->num_sendees;
    int total_sent = 0;
    fp->time_in = 0.00;
    fp->req.num_completed = 0;
    fp->req.num_pending = FP_BATCH_SIZE;
    fp->req.condition = CMCondition_get(fp_read_data->cm, NULL);

    for (i = 0; i<num_sendees; i++) {
	batchcount++;
	total_sent++;
		/* fprintf(stderr, "reader rank:%d:flush_data to writer:%d:of:%d:step:%d:batch:%d:total_sent:%d\n", */
		/* 	fp->rank, sendee, num_sendees, fp->mystep, batchcount, total_sent); */
	send_read_msg(fp, i, 0);

	if ((total_sent % FP_BATCH_SIZE == 0) || (total_sent == num_sendees)) {
            fp->req.num_pending = batchcount;

	    fp_verbose(fp, "in perform_reads, blocking on:%d:step:%d\n", fp->req.condition, fp->mystep);
            CMCondition_wait(fp_read_data->cm, fp->req.condition);
	    fp_verbose(fp, "after blocking:%d:step:%d\n", fp->req.condition, fp->mystep);
	    fp->req.num_completed = 0;
	    //fp->req.num_pending = 0;
	    //total_sent = 0;
            batchcount = 0;
            fp->req.condition = CMCondition_get(fp_read_data->cm, NULL);
	}

    }

    //Immediate_cleanup?
    free(fp->sendees);
    fp->sendees = NULL;
    fp->num_sendees = 0;

    
    //Get the current var_list
    pthread_mutex_lock(&(fp->queue_mutex));
    timestep_seperated_var_list * curr_var_list =  find_var_list(fp, fp->mystep);
    if(!curr_var_list)
    {
        fprintf(stderr, "Severe logic error!\n");
        exit(1);
    }
    flexpath_var *tmpvars = curr_var_list->var_list;
    pthread_mutex_unlock(&(fp->queue_mutex));

    while (tmpvars) {
	if (tmpvars->displ) {
	    free_displacements(tmpvars->displ, tmpvars->num_displ);
	    tmpvars->displ = NULL;
	}

	if (tmpvars->sel) {
	    a2sel_free(tmpvars->sel);
	    tmpvars->sel = NULL;
	}

	tmpvars = tmpvars->next;
    }
    fp_verbose(fp, "leaving perform_reads.\n");
    return 0;
}

int
adios_read_flexpath_inq_var_blockinfo(const ADIOS_FILE* fp,
				      ADIOS_VARINFO* varinfo)
{ /*log_debug( "flexpath:adios function inq var block info\n");*/ return 0; }

int
adios_read_flexpath_inq_var_stat(const ADIOS_FILE* fp,
				 ADIOS_VARINFO* varinfo,
				 int per_step_stat,
				 int per_block_stat)
{ /*log_debug( "flexpath:adios function inq var stat\n");*/ return 0; }


int
adios_read_flexpath_schedule_read_byid(const ADIOS_FILE *adiosfile,
				       const ADIOS_SELECTION *sel,
				       int varid,
				       int from_steps,
				       int nsteps,
				       void *data)
{
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    pthread_mutex_lock(&(fp->queue_mutex));
    timestep_seperated_var_list * curr_var_list =  find_var_list(fp, fp->mystep);
    if(!curr_var_list)
    {
        fprintf(stderr, "Severe logic error!\n");
        exit(1);
    }
    flexpath_var *fpvar = curr_var_list->var_list;
    pthread_mutex_unlock(&(fp->queue_mutex));

    fp_verbose(fp, "entering schedule_read_byid, varid %d\n", varid);
    while (fpvar) {
        if (fpvar->id == varid)
        	break;
        else
	    fpvar=fpvar->next;
    }
    if (!fpvar) {
        adios_error(err_invalid_varid,
		    "Invalid variable id: %d\n",
		    varid);
        return err_invalid_varid;
    }

    //store the user allocated buffer.
    flexpath_var_chunk *chunk = &fpvar->chunks[0];
    if (nsteps != 1) {
	adios_error (err_invalid_timestep,
                     "Only one step can be read from a stream at a time. "
                     "You requested %d steps in adios_schedule_read()\n",
		     nsteps);
        return err_invalid_timestep;
    }
    // this is done so that the user can do multiple schedule_read/perform_reads
    // within before doing release/advance step. Might need a better way to
    // manage the ADIOS selections.
    if (fpvar->sel) {
	a2sel_free(fpvar->sel);
	fpvar->sel = NULL;
    }
    if (!sel) { // null selection; read whole variable
	//TODO: This will have to be fixed for local arrays,
	// but dataspaces doesn't have local arrays so there
	// are no use cases for it.
	uint64_t *starts = calloc(fpvar->ndims, sizeof(uint64_t));
	uint64_t *counts = calloc(fpvar->ndims, sizeof(uint64_t));
	memcpy(counts, fpvar->global_dims, fpvar->ndims*sizeof(uint64_t));
	fpvar->sel = a2sel_boundingbox(fpvar->ndims, starts, counts);
    } else {
	fpvar->sel = a2sel_copy(sel);
    }

    switch(fpvar->sel->type)
    {
    case ADIOS_SELECTION_WRITEBLOCK:
    {
        chunk->user_buf = data;
        fpvar->start_position = 0;
	int writer_index = fpvar->sel->u.block.index;
	if (writer_index > fp->num_bridges) {
	    adios_error(err_out_of_bound,
			"No process exists on the writer side matching the index.\n");
	    return err_out_of_bound;
	}
	add_var_to_read_message(fp, writer_index, fpvar->varname);
	break;
    }
    case ADIOS_SELECTION_BOUNDINGBOX:
    {
        // boundingbox for a scalar; handle it as we do with inq_var
        if (fpvar->ndims == 0) {
	    if (chunk->has_data) {
		memcpy(data, chunk->data, fpvar->type_size);
	    } else {
		printf("Trying to schedule read on scalar %s, no data fpvar %p, chunk %p\n", fpvar->varname, fpvar, chunk);
	    }
        } else {
            if (fp->host_language == FP_FORTRAN_MODE) {
                reverse_dims(sel->u.bb.start, sel->u.bb.ndim);
                reverse_dims(sel->u.bb.count, sel->u.bb.ndim);
            }
            chunk->user_buf = data;
            fpvar->start_position = 0;
            free_displacements(fpvar->displ, fpvar->num_displ);
            fpvar->displ = NULL;
            int j=0;
            int need_count = 0;
            array_displacements *all_disp = NULL;
            uint64_t pos = 0;
            for (j=0; j<fp->num_bridges; j++) {
                int destination=0;
                evgroup * the_gp = fp->current_global_info;
                if (need_writer(fp, j, fpvar->sel, the_gp, fpvar->varname)==1) {
		    /* fprintf(stderr, "\t\trank: %d need_writer: %d\n", fp->rank, j); */
                    uint64_t _pos = 0;
                    need_count++;
                    destination = j;
                    global_var *gvar = find_gbl_var(the_gp->vars, fpvar->varname, the_gp->num_vars);
                    // displ is freed in release_step.
                    array_displacements *displ = get_writer_displacements(j, fpvar->sel, gvar, &_pos);
                    displ->pos = pos;
                    _pos *= (uint64_t)fpvar->type_size;
                    pos += _pos;

                    all_disp = realloc(all_disp, sizeof(array_displacements)*need_count);
                    all_disp[need_count-1] = *displ;
                    add_var_to_read_message(fp, j, fpvar->varname);
                }
            }
            fpvar->displ = all_disp;
            fpvar->num_displ = need_count;
        }
        break;
    }
    case ADIOS_SELECTION_AUTO:
    {
	adios_error(err_operation_not_supported,
		    "ADIOS_SELECTION_AUTO not yet supported by flexpath.");
	break;
    }
    case ADIOS_SELECTION_POINTS:
    {
	adios_error(err_operation_not_supported,
		    "ADIOS_SELECTION_POINTS not yet supported by flexpath.");
	break;
    }
    }
    fp_verbose(fp, "exiting schedule_read_byid\n");
    return 0;
}

int
adios_read_flexpath_schedule_read(const ADIOS_FILE *adiosfile,
			const ADIOS_SELECTION * sel,
			const char * varname,
			int from_steps,
			int nsteps,
			void * data)
{
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    fp_verbose(fp, "entering schedule_read, var: %s\n", varname);
    pthread_mutex_lock(&(fp->queue_mutex));
    timestep_seperated_var_list * curr_var_list =  find_var_list(fp, fp->mystep);
    if(!curr_var_list)
    {
        fprintf(stderr, "Severe logic error!\n");
        exit(1);
    }
    flexpath_var *fpvar = find_fp_var(curr_var_list->var_list, varname);
    pthread_mutex_unlock(&(fp->queue_mutex));

    if (!fpvar) {
        adios_error(err_invalid_varid,
		    "Invalid variable id: %s\n",
		    varname);
        return err_invalid_varid;
    }

    int id = fpvar->id;

    fp_verbose(fp, "Calling read_by_id, var: %d\n", id);
    return adios_read_flexpath_schedule_read_byid(adiosfile, sel, id, from_steps, nsteps, data);
}

int
adios_read_flexpath_get_attr (int *gp, const char *attrname,
                                 enum ADIOS_DATATYPES *type,
                                 int *size, void **data)
{
    //log_debug( "debug: adios_read_flexpath_get_attr\n");
    // TODO: borrowed from dimes
    adios_error(err_invalid_read_method,
		"adios_read_flexpath_get_attr is not implemented.");
    *size = 0;
    *type = adios_unknown;
    *data = 0;
    return adios_errno;
}

int
adios_read_flexpath_get_attr_byid (const ADIOS_FILE *adiosfile, int attrid,
				   enum ADIOS_DATATYPES *type,
				   int *size, void **data)
{
//    log_debug( "debug: adios_read_flexpath_get_attr_byid\n");
    // TODO: borrowed from dimes
    adios_error(err_invalid_read_method,
		"adios_read_flexpath_get_attr_byid is not implemented.");
    *size = 0;
    *type = adios_unknown;
    *data = 0;
    return adios_errno;
}

ADIOS_VARINFO*
adios_read_flexpath_inq_var(const ADIOS_FILE * adiosfile, const char* varname)
{
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    ADIOS_VARINFO *v = NULL;

    fp_verbose(fp, "entering flexpath_inq_var\n");
    pthread_mutex_lock(&(fp->queue_mutex));
    timestep_seperated_var_list * ts_var_list = find_var_list(fp, fp->mystep);
    flexpath_var *fpvar = find_fp_var(ts_var_list->var_list, varname);
    pthread_mutex_unlock(&(fp->queue_mutex));
    if (fpvar) {
        v = calloc(1, sizeof(ADIOS_VARINFO));

        if (!v) {
            adios_error(err_no_memory,
                        "Cannot allocate buffer in adios_read_flexpath_inq_var()");
            return NULL;
        }

	v = convert_var_info(fpvar, v, varname, adiosfile);
	fp_verbose(fp, "leaving flexpath_inq_var\n");
    }
    else {
        adios_error(err_invalid_varname, "Cannot find var %s\n", varname);
    }
    return v;
}

ADIOS_VARINFO*
adios_read_flexpath_inq_var_byid (const ADIOS_FILE * adiosfile, int varid)
{
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    fp_verbose(fp, "entering flexpath_inq_var_byid\n");
    if (varid >= 0 && varid < adiosfile->nvars) {
	ADIOS_VARINFO *v = adios_read_flexpath_inq_var(adiosfile, adiosfile->var_namelist[varid]);
	fp_verbose(fp, "leaving flexpath_inq_var_byid\n");
	return v;
    }
    else {
        adios_error(err_invalid_varid, "FLEXPATH method: Cannot find var %d\n", varid);
        return NULL;
    }
}

void
adios_read_flexpath_free_varinfo (ADIOS_VARINFO *adiosvar)
{
    //log_debug( "debug: adios_read_flexpath_free_varinfo\n");
    fprintf(stderr, "adios_read_flexpath_free_varinfo called\n");
    return;
}


ADIOS_TRANSINFO*
adios_read_flexpath_inq_var_transinfo(const ADIOS_FILE *gp, const ADIOS_VARINFO *vi)
{
    //adios_error(err_operation_not_supported, "Flexpath does not yet support transforms: var_transinfo.\n");
    ADIOS_TRANSINFO *trans = malloc(sizeof(ADIOS_TRANSINFO));
    memset(trans, 0, sizeof(ADIOS_TRANSINFO));
    trans->transform_type = adios_transform_none;
    return trans;
}


int
adios_read_flexpath_inq_var_trans_blockinfo(const ADIOS_FILE *gp, const ADIOS_VARINFO *vi, ADIOS_TRANSINFO *ti)
{
    adios_error(err_operation_not_supported, "Flexpath does not yet support transforms: trans_blockinfo.\n");
    return (int64_t)0;
}

int  adios_read_flexpath_get_dimension_order (const ADIOS_FILE *adiosfile)
{
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    return (fp->host_language == FP_FORTRAN_MODE);
}

void
adios_read_flexpath_reset_dimension_order (const ADIOS_FILE *adiosfile, int is_fortran)
{
    //log_debug( "debug: adios_read_flexpath_reset_dimension_order\n");
    adios_error(err_invalid_read_method, "adios_read_flexpath_reset_dimension_order is not implemented.");
}
