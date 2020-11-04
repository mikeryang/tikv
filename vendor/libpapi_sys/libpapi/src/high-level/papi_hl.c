/****************************/
/* THIS IS OPEN SOURCE CODE */
/****************************/

/**
* @file     papi_hl.c
* @author   Frank Winkler
*           frank.winkler@icl.utk.edu
* @author   Philip Mucci
*           mucci@cs.utk.edu
* @brief This file contains the 'high level' interface to PAPI.
*  BASIC is a high level language. ;-) */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <search.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include "papi.h"
#include "papi_internal.h"


/* For dynamic linking to libpapi */
/* Weak symbol for pthread_once to avoid additional linking
 * against libpthread when not used. */
#pragma weak pthread_once

#define verbose_fprintf \
   if (verbosity == 1) fprintf

/* defaults for number of components and events */
#define PAPIHL_NUM_OF_COMPONENTS 10
#define PAPIHL_NUM_OF_EVENTS_PER_COMPONENT 10

#define PAPIHL_ACTIVE 1
#define PAPIHL_DEACTIVATED 0

/* global components data begin *****************************************/
typedef struct components
{
   int component_id;
   int num_of_events;
   int max_num_of_events;
   char **event_names;
   int *event_codes;
   short *event_types;
   int EventSet; //only for testing at initialization phase
} components_t;

components_t *components = NULL;
int num_of_components = 0;
int max_num_of_components = PAPIHL_NUM_OF_COMPONENTS;
int total_num_events = 0;
int num_of_cleaned_threads = 0;

/* global components data end *******************************************/


/* thread local components data begin ***********************************/
typedef struct local_components
{
   int EventSet;
   /** Return values for the eventsets */
   long_long *values;
} local_components_t;

PAPI_TLS_KEYWORD local_components_t *_local_components = NULL;
PAPI_TLS_KEYWORD long_long _local_cycles;
PAPI_TLS_KEYWORD volatile bool _local_state = PAPIHL_ACTIVE;
PAPI_TLS_KEYWORD int _local_region_begin_cnt = 0; /**< Count each PAPI_hl_region_begin call */
PAPI_TLS_KEYWORD int _local_region_end_cnt = 0;   /**< Count each PAPI_hl_region_end call */

/* thread local components data end *************************************/


/* global event storage data begin **************************************/
typedef struct reads
{
   struct reads *next;
   struct reads *prev;
   long_long value;        /**< Event value */
} reads_t;

typedef struct
{
   long_long offset;       /**< Event value for region_begin */
   long_long total;        /**< Event value for region_end - region_begin + previous value */
   reads_t *read_values;   /**< List of read event values inside a region */
} value_t;

typedef struct regions
{
   char *region;           /**< Region name */
   struct regions *next;
   struct regions *prev;
   value_t values[];       /**< Array of event values based on current eventset */
} regions_t;

typedef struct
{
   unsigned long key;      /**< Thread ID */
   regions_t *value;       /**< List of regions */
} threads_t;

int compar(const void *l, const void *r)
{
   const threads_t *lm = l;
   const threads_t *lr = r;
   return lm->key - lr->key;
}

typedef struct
{
   void *root;             /**< Root of binary tree */
   threads_t *find_p;      /**< Pointer that is used for finding a thread node */ 
} binary_tree_t;

/**< Global binary tree that stores events from all threads */
binary_tree_t* binary_tree = NULL;

/* global event storage data end ****************************************/


/* global auxiliary variables begin *************************************/
enum region_type { REGION_BEGIN, REGION_READ, REGION_END };

char **requested_event_names = NULL; /**< Events from user or default */
int num_of_requested_events = 0;

bool hl_initiated = false;       /**< Check PAPI-HL has been initiated */
bool hl_finalized = false;       /**< Check PAPI-HL has been fininalized */
bool events_determined = false;  /**< Check if events are determined */
bool output_generated = false;   /**< Check if output has been already generated */
static char *absolute_output_file_path = NULL;
static int output_counter = 0;   /**< Count each output generation. Not used yet */
short verbosity = 1;             /**< Verbose output is always generated */
bool state = PAPIHL_ACTIVE;      /**< PAPIHL is active until first error or finalization */
static int region_begin_cnt = 0; /**< Count each PAPI_hl_region_begin call */
static int region_end_cnt = 0;   /**< Count each PAPI_hl_region_end call */
unsigned long master_thread_id = -1; /**< Remember id of master thread */

/* global auxiliary variables end ***************************************/

static void _internal_hl_library_init(void);
static void _internal_hl_onetime_library_init(void);

/* functions for creating eventsets for different components */
static int _internal_hl_checkCounter ( char* counter );
static int _internal_hl_determine_rank();
static char *_internal_hl_remove_spaces( char *str );
static int _internal_hl_determine_default_events();
static int _internal_hl_read_user_events();
static int _internal_hl_new_component(int component_id, components_t *component);
static int _internal_hl_add_event_to_component(char *event_name, int event,
                                        short event_type, components_t *component);
static int _internal_hl_create_components();
static int _internal_hl_read_events(const char* events);
static int _internal_hl_create_event_sets();

/* functions for storing events */
static inline reads_t* _internal_hl_insert_read_node( reads_t** head_node );
static inline int _internal_hl_add_values_to_region( regions_t *node, enum region_type reg_typ );
static inline regions_t* _internal_hl_insert_region_node( regions_t** head_node, const char *region );
static inline regions_t* _internal_hl_find_region_node( regions_t* head_node, const char *region );
static inline threads_t* _internal_hl_insert_thread_node( unsigned long tid );
static inline threads_t* _internal_hl_find_thread_node( unsigned long tid );
static int _internal_hl_store_counters( unsigned long tid, const char *region,
                                        enum region_type reg_typ );
static int _internal_hl_read_counters();
static int _internal_hl_read_and_store_counters( const char *region, enum region_type reg_typ );
static int _internal_hl_create_global_binary_tree();

/* functions for output generation */
static int _internal_hl_mkdir(const char *dir);
static int _internal_hl_determine_output_path();
static void _internal_hl_json_line_break_and_indent(FILE* f, bool b, int width);
static void _internal_hl_json_region_events(FILE* f, bool beautifier, regions_t *regions);
static void _internal_hl_json_regions(FILE* f, bool beautifier, threads_t* thread_node);
static void _internal_hl_json_threads(FILE* f, bool beautifier, unsigned long* tids, int threads_num);
static void _internal_hl_write_output();

/* functions for cleaning up heap memory */
static void _internal_hl_clean_up_local_data();
static void _internal_hl_clean_up_global_data();
static void _internal_hl_clean_up_all(bool deactivate);
static int _internal_hl_check_for_clean_thread_states();

/* internal advanced functions */
int _internal_PAPI_hl_init(); /**< intialize high level library */
int _internal_PAPI_hl_cleanup_thread(); /**< clean local-thread event sets */
int _internal_PAPI_hl_finalize(); /**< shutdown event sets and clear up everything */
int _internal_PAPI_hl_set_events(const char* events); /**< set specfic events to be recorded */
void _internal_PAPI_hl_print_output(); /**< generate output */


static void _internal_hl_library_init(void)
{
   /* This function is only called by one thread! */
   int retval;

   /* check VERBOSE level */
   if ( getenv("PAPI_NO_WARNING") != NULL ) {
      verbosity = 0;
   }

   if ( ( retval = PAPI_library_init(PAPI_VER_CURRENT) ) != PAPI_VER_CURRENT )
      verbose_fprintf(stdout, "PAPI-HL Error: PAPI_library_init failed!\n");
   
   /* PAPI_thread_init only suceeds if PAPI_library_init has suceeded */
   if ((retval = PAPI_thread_init(&pthread_self)) == PAPI_OK) {

      /* determine output directory and output file */
      if ( ( retval = _internal_hl_determine_output_path() ) != PAPI_OK ) {
         verbose_fprintf(stdout, "PAPI-HL Error: _internal_hl_determine_output_path failed!\n");
         state = PAPIHL_DEACTIVATED;
         verbose_fprintf(stdout, "PAPI-HL Error: PAPI could not be initiated!\n");
      } else {

         /* register the termination function for output */
         atexit(_internal_PAPI_hl_print_output);
         verbose_fprintf(stdout, "PAPI-HL Info: PAPI has been initiated!\n");

         /* remember thread id */
         master_thread_id = PAPI_thread_id();
         HLDBG("master_thread_id=%lu\n", master_thread_id);
      }
   } else {
      verbose_fprintf(stdout, "PAPI-HL Error: PAPI_thread_init failed!\n");
      state = PAPIHL_DEACTIVATED;
      verbose_fprintf(stdout, "PAPI-HL Error: PAPI could not be initiated!\n");
   }

   /* Support multiplexing if user wants to */
   if ( getenv("PAPI_MULTIPLEX") != NULL ) {
      retval = PAPI_multiplex_init();
      if ( retval == PAPI_ENOSUPP) {
         verbose_fprintf(stdout, "PAPI-HL Info: Multiplex is not supported!\n");
      } else if ( retval != PAPI_OK ) {
         verbose_fprintf(stdout, "PAPI-HL Error: PAPI_multiplex_init failed!\n");
      } else if ( retval == PAPI_OK ) {
         verbose_fprintf(stdout, "PAPI-HL Info: Multiplex has been initiated!\n");
      }
   }

   hl_initiated = true;
}

static void _internal_hl_onetime_library_init(void)
{
   static pthread_once_t library_is_initialized = PTHREAD_ONCE_INIT;
   if ( pthread_once ) {
      /* we assume that PAPI_hl_init() is called from a parallel region */
      pthread_once(&library_is_initialized, _internal_hl_library_init);
      /* wait until first thread has finished */
      int i = 0;
      /* give it 5 seconds in case PAPI_thread_init crashes */
      while ( !hl_initiated && (i++) < 500000 )
         usleep(10);
   } else {
      /* we assume that PAPI_hl_init() is called from a serial application
       * that was not linked against libpthread */
      _internal_hl_library_init();
   }
}

static int
_internal_hl_checkCounter ( char* counter )
{
   int EventSet = PAPI_NULL;
   int eventcode;
   int retval;

   HLDBG("Counter: %s\n", counter);
   if ( ( retval = PAPI_create_eventset( &EventSet ) ) != PAPI_OK )
      return ( retval );

   if ( ( retval = PAPI_event_name_to_code( counter, &eventcode ) ) != PAPI_OK ) {
      HLDBG("Counter %s does not exist\n", counter);
      return ( retval );
   }

   if ( ( retval = PAPI_add_event (EventSet, eventcode) ) != PAPI_OK ) {
      HLDBG("Cannot add counter %s\n", counter);
      return ( retval );
   }

   if ( ( retval = PAPI_cleanup_eventset (EventSet) ) != PAPI_OK )
      return ( retval );

   if ( ( retval = PAPI_destroy_eventset (&EventSet) ) != PAPI_OK )
      return ( retval );

   return ( PAPI_OK );
}

static int _internal_hl_determine_rank()
{
   int rank = -1;
   /* check environment variables for rank identification */

   if ( getenv("OMPI_COMM_WORLD_RANK") != NULL )
      rank = atoi(getenv("OMPI_COMM_WORLD_RANK"));
   else if ( getenv("ALPS_APP_PE") != NULL )
      rank = atoi(getenv("ALPS_APP_PE"));
   else if ( getenv("PMI_RANK") != NULL )
      rank = atoi(getenv("PMI_RANK"));
   else if ( getenv("SLURM_PROCID") != NULL )
      rank = atoi(getenv("SLURM_PROCID"));

   return rank;
}

static char *_internal_hl_remove_spaces( char *str )
{
   char *out = str, *put = str;
   for(; *str != '\0'; ++str) {
      if(*str != ' ')
         *put++ = *str;
   }
   *put = '\0';
   return out;
}

static int _internal_hl_determine_default_events()
{
   int i;
   HLDBG("Default events\n");
   char *default_events[] = {
      "perf::TASK-CLOCK",
      "PAPI_TOT_INS",
      "PAPI_TOT_CYC",
      "PAPI_FP_INS",
      "PAPI_FP_OPS"
   };
   int num_of_defaults = sizeof(default_events) / sizeof(char*);

   /* allocate memory for requested events */
   requested_event_names = (char**)malloc(num_of_defaults * sizeof(char*));
   if ( requested_event_names == NULL )
      return ( PAPI_ENOMEM );

   /* check if default events are available on the current machine */
   for ( i = 0; i < num_of_defaults; i++ ) {
      if ( _internal_hl_checkCounter( default_events[i] ) == PAPI_OK ) {
         requested_event_names[num_of_requested_events++] = strdup(default_events[i]);
         if ( requested_event_names[num_of_requested_events -1] == NULL )
            return ( PAPI_ENOMEM );
      }
   }

   return ( PAPI_OK );
}

static int _internal_hl_read_user_events(const char *user_events)
{
   char* user_events_copy;
   const char *separator; //separator for events
   int num_of_req_events = 1; //number of events in string
   int req_event_index = 0; //index of event
   const char *position = NULL; //current position in processed string
   char *token;
   
   HLDBG("User events: %s\n", user_events);
   user_events_copy = strdup(user_events);
   if ( user_events_copy == NULL )
      return ( PAPI_ENOMEM );

   /* check if string is not empty */
   if ( strlen( user_events_copy ) > 0 )
   {
      /* count number of separator characters */
      position = user_events_copy;
      separator=",";
      while ( *position ) {
         if ( strchr( separator, *position ) ) {
            num_of_req_events++;
         }
            position++;
      }

      /* allocate memory for requested events */
      requested_event_names = (char**)malloc(num_of_req_events * sizeof(char*));
      if ( requested_event_names == NULL )
         return ( PAPI_ENOMEM );

      /* parse list of event names */
      token = strtok( user_events_copy, separator );
      while ( token ) {
         if ( req_event_index >= num_of_req_events ){
            /* more entries as in the first run */
            return PAPI_EINVAL;
         }
         requested_event_names[req_event_index] = strdup(_internal_hl_remove_spaces(token));
         if ( requested_event_names[req_event_index] == NULL )
            return ( PAPI_ENOMEM );
         token = strtok( NULL, separator );
         req_event_index++;
      }
   }

   num_of_requested_events = num_of_req_events;
   free(user_events_copy);
   if ( num_of_requested_events == 0 )
      return PAPI_EINVAL;

   HLDBG("Number of requested events: %d\n", num_of_requested_events);
   return ( PAPI_OK );
}

static int _internal_hl_new_component(int component_id, components_t *component)
{
   int retval;

   /* create new EventSet */
   component->EventSet = PAPI_NULL;
   if ( ( retval = PAPI_create_eventset( &component->EventSet ) ) != PAPI_OK ) {
      verbose_fprintf(stdout, "PAPI-HL Error: Cannot create EventSet for component %d.\n", component_id);
      return ( retval );
   }

   /* Support multiplexing if user wants to */
   if ( getenv("PAPI_MULTIPLEX") != NULL ) {

      /* multiplex only for cpu core events */
      if ( component_id == 0 ) {
         retval = PAPI_assign_eventset_component(component->EventSet, component_id);
         if ( retval != PAPI_OK ) {
            verbose_fprintf(stdout, "PAPI-HL Error: PAPI_assign_eventset_component failed.\n");
         } else {
            if ( PAPI_get_multiplex(component->EventSet) == false ) {
               retval = PAPI_set_multiplex(component->EventSet);
               if ( retval != PAPI_OK ) {
                  verbose_fprintf(stdout, "PAPI-HL Error: PAPI_set_multiplex failed.\n");
               }
            }
         }
      }
   }

   component->component_id = component_id;
   component->num_of_events = 0;
   component->max_num_of_events = PAPIHL_NUM_OF_EVENTS_PER_COMPONENT;

   component->event_names = NULL;
   component->event_names = (char**)malloc(component->max_num_of_events * sizeof(char*));
   if ( component->event_names == NULL )
      return ( PAPI_ENOMEM );

   component->event_codes = NULL;
   component->event_codes = (int*)malloc(component->max_num_of_events * sizeof(int));
   if ( component->event_codes == NULL )
      return ( PAPI_ENOMEM );

   component->event_types = NULL;
   component->event_types = (short*)malloc(component->max_num_of_events * sizeof(short));
   if ( component->event_types == NULL )
      return ( PAPI_ENOMEM );

   num_of_components += 1;
   return ( PAPI_OK );
}

static int _internal_hl_add_event_to_component(char *event_name, int event,
                                        short event_type, components_t *component)
{
   int i, retval;

   /* check if we need to reallocate memory for event_names, event_codes and event_types */
   if ( component->num_of_events == component->max_num_of_events ) {
      component->max_num_of_events *= 2;

      component->event_names = (char**)realloc(component->event_names, component->max_num_of_events * sizeof(char*));
      if ( component->event_names == NULL )
         return ( PAPI_ENOMEM );

      component->event_codes = (int*)realloc(component->event_codes, component->max_num_of_events * sizeof(int));
      if ( component->event_codes == NULL )
         return ( PAPI_ENOMEM );

      component->event_types = (short*)realloc(component->event_types, component->max_num_of_events * sizeof(short));
      if ( component->event_types == NULL )
         return ( PAPI_ENOMEM );
   }

   retval = PAPI_add_event( component->EventSet, event );
   if ( retval != PAPI_OK ) {
      const PAPI_component_info_t* cmpinfo;
      cmpinfo = PAPI_get_component_info( component->component_id );
      verbose_fprintf(stdout, "PAPI-HL Warning: Cannot add %s to component %s.\n", event_name, cmpinfo->name);
      verbose_fprintf(stdout, "The following event combination is not supported:\n");
      for ( i = 0; i < component->num_of_events; i++ )
         verbose_fprintf(stdout, "  %s\n", component->event_names[i]);
      verbose_fprintf(stdout, "  %s\n", event_name);
      verbose_fprintf(stdout, "Advice: Use papi_event_chooser to obtain an appropriate event set for this component or set PAPI_MULTIPLEX=1.\n");

      return PAPI_EINVAL;
   }

   component->event_names[component->num_of_events] = event_name;
   component->event_codes[component->num_of_events] = event;
   component->event_types[component->num_of_events] = event_type;
   component->num_of_events += 1;

   total_num_events += 1;

   return PAPI_OK;
}

static int _internal_hl_create_components()
{
   int i, j, retval, event;
   int component_id = -1;
   int comp_index = 0;
   bool component_exists = false;
   short event_type = 0;

   HLDBG("Create components\n");
   components = (components_t*)malloc(max_num_of_components * sizeof(components_t));
   if ( components == NULL )
      return ( PAPI_ENOMEM );

   for ( i = 0; i < num_of_requested_events; i++ ) {
      /* check if requested event contains event type (instant or delta) */
      const char sep = '=';
      char *ret;
      int index;
      /* search for '=' in event name */
      ret = strchr(requested_event_names[i], sep);
      if (ret) {
         if ( strcmp(ret, "=instant") == 0 )
            event_type = 1;
         else
            event_type = 0;

         /* get index of '=' in event name */
         index = (int)(ret - requested_event_names[i]);
         /* remove event type from string if '=instant' or '=delta' */
         if ( (strcmp(ret, "=instant") == 0) || (strcmp(ret, "=delta") == 0) )
            requested_event_names[i][index] = '\0';
      }

      /* check if event is supported on current machine */
      retval = _internal_hl_checkCounter(requested_event_names[i]);
      if ( retval != PAPI_OK ) {
         verbose_fprintf(stdout, "PAPI-HL Warning: \"%s\" does not exist or is not supported on this machine.\n", requested_event_names[i]);
      } else {
         /* determine event code and corresponding component id */
         retval = PAPI_event_name_to_code( requested_event_names[i], &event );
         if ( retval != PAPI_OK )
            return ( retval );
         component_id = PAPI_COMPONENT_INDEX( event );

         /* check if component_id already exists in global components structure */
         for ( j = 0; j < num_of_components; j++ ) {
            if ( components[j].component_id == component_id ) {
               component_exists = true;
               comp_index = j;
               break;
            }
            else {
               component_exists = false;
            }
         }

         /* create new component */
         if ( false == component_exists ) {
            /* check if we need to reallocate memory for components */
            if ( num_of_components == max_num_of_components ) {
               max_num_of_components *= 2;
               components = (components_t*)realloc(components, max_num_of_components * sizeof(components_t));
               if ( components == NULL )
                  return ( PAPI_ENOMEM );
            }
            comp_index = num_of_components;
            retval = _internal_hl_new_component(component_id, &components[comp_index]);
            if ( retval != PAPI_OK )
               return ( retval );
         }

         /* add event to current component */
         retval = _internal_hl_add_event_to_component(requested_event_names[i], event, event_type, &components[comp_index]);
         if ( retval == PAPI_ENOMEM )
            return ( retval );
      }
   }

   HLDBG("Number of components %d\n", num_of_components);
   if ( num_of_components > 0 )
      verbose_fprintf(stdout, "PAPI-HL Info: Using the following events:\n");

   /* destroy all EventSets from global data */
   for ( i = 0; i < num_of_components; i++ ) {
      if ( ( retval = PAPI_cleanup_eventset (components[i].EventSet) ) != PAPI_OK )
         return ( retval );
      if ( ( retval = PAPI_destroy_eventset (&components[i].EventSet) ) != PAPI_OK )
         return ( retval );
      components[i].EventSet = PAPI_NULL;

      HLDBG("component_id = %d\n", components[i].component_id);
      HLDBG("num_of_events = %d\n", components[i].num_of_events);
      for ( j = 0; j < components[i].num_of_events; j++ ) {
         HLDBG(" %s type=%d\n", components[i].event_names[j], components[i].event_types[j]);
         verbose_fprintf(stdout, "  %s\n", components[i].event_names[j]);
      }
   }

   if ( num_of_components == 0 )
      return PAPI_EINVAL;

   return PAPI_OK;
}

static int _internal_hl_read_events(const char* events)
{
   int i, retval;
   HLDBG("Read events: %s\n", events);
   if ( events != NULL ) {
      if ( _internal_hl_read_user_events(events) != PAPI_OK )
         if ( ( retval = _internal_hl_determine_default_events() ) != PAPI_OK )
            return ( retval );

   /* check if user specified events via environment variable */
   } else if ( getenv("PAPI_EVENTS") != NULL ) {
      char *user_events_from_env = strdup( getenv("PAPI_EVENTS") );
      if ( user_events_from_env == NULL )
         return ( PAPI_ENOMEM );
      if ( _internal_hl_read_user_events(user_events_from_env) != PAPI_OK )
         if ( ( retval = _internal_hl_determine_default_events() ) != PAPI_OK ) {
            free(user_events_from_env);
            return ( retval );
         }
      free(user_events_from_env);
   } else {
      if ( ( retval = _internal_hl_determine_default_events() ) != PAPI_OK )
            return ( retval );
   }

   /* create components based on requested events */
   if ( _internal_hl_create_components() != PAPI_OK )
   {
      /* requested events do not work at all, use default events */
      verbose_fprintf(stdout, "PAPI-HL Warning: All requested events do not work, using default.\n");

      for ( i = 0; i < num_of_requested_events; i++ )
         free(requested_event_names[i]);
      free(requested_event_names);
      num_of_requested_events = 0;
      if ( ( retval = _internal_hl_determine_default_events() ) != PAPI_OK )
         return ( retval );
      if ( ( retval = _internal_hl_create_components() ) != PAPI_OK )
         return ( retval );
   }

   events_determined = true;
   return ( PAPI_OK );
}

static int _internal_hl_create_event_sets()
{
   int i, j, retval;
   long_long cycles;

   if ( state == PAPIHL_ACTIVE ) {
      /* allocate memory for local components */
      _local_components = (local_components_t*)malloc(num_of_components * sizeof(local_components_t));
      if ( _local_components == NULL )
         return ( PAPI_ENOMEM );

      for ( i = 0; i < num_of_components; i++ ) {
         /* create EventSet */
         _local_components[i].EventSet = PAPI_NULL;
         if ( ( retval = PAPI_create_eventset( &_local_components[i].EventSet ) ) != PAPI_OK ) {
            return (retval );
         }

         /* Support multiplexing if user wants to */
         if ( getenv("PAPI_MULTIPLEX") != NULL ) {

            /* multiplex only for cpu core events */
            if ( components[i].component_id == 0 ) {
               retval = PAPI_assign_eventset_component(_local_components[i].EventSet, components[i].component_id );
	            if ( retval != PAPI_OK ) {
		            verbose_fprintf(stdout, "PAPI-HL Error: PAPI_assign_eventset_component failed.\n");
               } else {
                  if ( PAPI_get_multiplex(_local_components[i].EventSet) == false ) {
                     retval = PAPI_set_multiplex(_local_components[i].EventSet);
                     if ( retval != PAPI_OK ) {
		                  verbose_fprintf(stdout, "PAPI-HL Error: PAPI_set_multiplex failed.\n");
                     }
                  }
               }
            }
         }

         /* add event to current EventSet */
         for ( j = 0; j < components[i].num_of_events; j++ ) {
            retval = PAPI_add_event( _local_components[i].EventSet, components[i].event_codes[j] );
            if ( retval != PAPI_OK ) {
               return (retval );
            }
         }
         /* allocate memory for return values */
         _local_components[i].values = (long_long*)malloc(components[i].num_of_events * sizeof(long_long));
         if ( _local_components[i].values == NULL )
            return ( PAPI_ENOMEM );

      }

      for ( i = 0; i < num_of_components; i++ ) {
         if ( ( retval = PAPI_start( _local_components[i].EventSet ) ) != PAPI_OK )
            return (retval );

         /* warm up PAPI code paths and data structures */
         if ( ( retval = PAPI_read_ts( _local_components[i].EventSet, _local_components[i].values, &cycles ) != PAPI_OK ) ) {
            return (retval );
         }
      }
      return PAPI_OK;
   }
   return ( PAPI_EMISC );
}

static inline reads_t* _internal_hl_insert_read_node(reads_t** head_node)
{
   reads_t *new_node;

   /* create new region node */
   if ( ( new_node = malloc(sizeof(reads_t)) ) == NULL )
      return ( NULL );
   new_node->next = NULL;
   new_node->prev = NULL;

   /* insert node in list */
   if ( *head_node == NULL ) {
      *head_node = new_node;
      return new_node;
   }
   (*head_node)->prev = new_node;
   new_node->next = *head_node;
   *head_node = new_node;

   return new_node;
}

static inline int _internal_hl_add_values_to_region( regions_t *node, enum region_type reg_typ )
{
   int i, j;
   int region_count = 1;
   int cmp_iter = 2;

   if ( reg_typ == REGION_BEGIN ) {
      /* set first fixed counters */
      node->values[0].offset = region_count;
      node->values[1].offset = _local_cycles;
      /* events from components */
      for ( i = 0; i < num_of_components; i++ )
         for ( j = 0; j < components[i].num_of_events; j++ )
            node->values[cmp_iter++].offset = _local_components[i].values[j];
   } else if ( reg_typ == REGION_READ ) {
      /* create a new read node and add values*/
      reads_t* read_node;
      if ( ( read_node = _internal_hl_insert_read_node(&node->values[1].read_values) ) == NULL )
         return ( PAPI_ENOMEM );
      read_node->value = _local_cycles - node->values[1].offset;
      for ( i = 0; i < num_of_components; i++ ) {
         for ( j = 0; j < components[i].num_of_events; j++ ) {
            reads_t* read_node;
            if ( ( read_node = _internal_hl_insert_read_node(&node->values[cmp_iter].read_values) ) == NULL )
               return ( PAPI_ENOMEM );
            if ( components[i].event_types[j] == 1 )
               read_node->value = _local_components[i].values[j];
            else
               read_node->value = _local_components[i].values[j] - node->values[cmp_iter].offset;
            cmp_iter++;
         }
      }
   } else if ( reg_typ == REGION_END ) {
      /* determine difference of current value and offset and add
         previous total value */
      node->values[0].total += node->values[0].offset;
      node->values[1].total += _local_cycles - node->values[1].offset;
      /* events from components */
      for ( i = 0; i < num_of_components; i++ )
         for ( j = 0; j < components[i].num_of_events; j++ ) {
            /* if event type is istant only save last value */
            if ( components[i].event_types[j] == 1 )
               node->values[cmp_iter].total += _local_components[i].values[j];
            else
               node->values[cmp_iter].total += _local_components[i].values[j] - node->values[cmp_iter].offset;
            cmp_iter++;
         }
   }
   return ( PAPI_OK );
}


static inline regions_t* _internal_hl_insert_region_node(regions_t** head_node, const char *region )
{
   regions_t *new_node;
   int i;
   int extended_total_num_events;

   /* number of all events including region count and CPU cycles */
   extended_total_num_events = total_num_events + 2;

   /* create new region node */
   new_node = malloc(sizeof(regions_t) + extended_total_num_events * sizeof(value_t));
   if ( new_node == NULL )
      return ( NULL );
   new_node->region = (char *)malloc((strlen(region) + 1) * sizeof(char));
   if ( new_node->region == NULL )
      return ( NULL );
   new_node->next = NULL;
   new_node->prev = NULL;
   strcpy(new_node->region, region);
   for ( i = 0; i < extended_total_num_events; i++ ) {
      new_node->values[i].total = 0;
      new_node->values[i].read_values = NULL;
   }

   /* insert node in list */
   if ( *head_node == NULL ) {
      *head_node = new_node;
      return new_node;
   }
   (*head_node)->prev = new_node;
   new_node->next = *head_node;
   *head_node = new_node;

   return new_node;
}


static inline regions_t* _internal_hl_find_region_node(regions_t* head_node, const char *region )
{
   regions_t* find_node = head_node;
   while ( find_node != NULL ) {
      if ( strcmp(find_node->region, region) == 0 ) {
         return find_node;
      }
      find_node = find_node->next;
   }
   find_node = NULL;
   return find_node;
}

static inline threads_t* _internal_hl_insert_thread_node(unsigned long tid)
{
   threads_t *new_node = (threads_t*)malloc(sizeof(threads_t));
   if ( new_node == NULL )
      return ( NULL );
   new_node->key = tid;
   new_node->value = NULL; /* head node of region list */
   tsearch(new_node, &binary_tree->root, compar);
   return new_node;
}

static inline threads_t* _internal_hl_find_thread_node(unsigned long tid)
{
   threads_t *find_node = binary_tree->find_p;
   find_node->key = tid;
   void *found = tfind(find_node, &binary_tree->root, compar);
   if ( found != NULL ) {
      find_node = (*(threads_t**)found);
      return find_node;
   }
   return NULL;
}


static int _internal_hl_store_counters( unsigned long tid, const char *region,
                                        enum region_type reg_typ )
{
   int retval;

   _papi_hwi_lock( HIGHLEVEL_LOCK );
   threads_t* current_thread_node;

   /* check if current thread is already stored in tree */
   current_thread_node = _internal_hl_find_thread_node(tid);
   if ( current_thread_node == NULL ) {
      /* insert new node for current thread in tree if type is REGION_BEGIN */
      if ( reg_typ == REGION_BEGIN ) {
         if ( ( current_thread_node = _internal_hl_insert_thread_node(tid) ) == NULL ) {
            _papi_hwi_unlock( HIGHLEVEL_LOCK );
            return ( PAPI_ENOMEM );
         }
      } else {
         _papi_hwi_unlock( HIGHLEVEL_LOCK );
         return ( PAPI_EINVAL );
      }
   }

   regions_t* current_region_node;
   /* check if node for current region already exists */
   current_region_node = _internal_hl_find_region_node(current_thread_node->value, region);

   if ( current_region_node == NULL ) {
      /* create new node for current region in list if type is REGION_BEGIN */
      if ( reg_typ == REGION_BEGIN ) {
         if ( ( current_region_node = _internal_hl_insert_region_node(&current_thread_node->value,region) ) == NULL ) {
            _papi_hwi_unlock( HIGHLEVEL_LOCK );
            return ( PAPI_ENOMEM );
         }
      } else {
         /* ignore no matching REGION_READ */
         if ( reg_typ == REGION_READ ) {
            verbose_fprintf(stdout, "PAPI-HL Warning: Cannot find matching region for PAPI_hl_read(\"%s\") for thread id=%lu.\n", region, PAPI_thread_id());
            retval = PAPI_OK;
         } else {
            verbose_fprintf(stdout, "PAPI-HL Warning: Cannot find matching region for PAPI_hl_region_end(\"%s\") for thread id=%lu.\n", region, PAPI_thread_id());
            retval = PAPI_EINVAL;
         }
         _papi_hwi_unlock( HIGHLEVEL_LOCK );
         return ( retval );
      }
   }

   /* add recorded values to current region */
   if ( ( retval = _internal_hl_add_values_to_region( current_region_node, reg_typ ) ) != PAPI_OK ) {
      _papi_hwi_unlock( HIGHLEVEL_LOCK );
      return ( retval );
   }

   /* count all REGION_BEGIN and REGION_END calls */
   if ( reg_typ == REGION_BEGIN ) region_begin_cnt++;
   if ( reg_typ == REGION_END ) region_end_cnt++;

   _papi_hwi_unlock( HIGHLEVEL_LOCK );
   return ( PAPI_OK );
}


static int _internal_hl_read_counters()
{
   int i, j, retval;
   for ( i = 0; i < num_of_components; i++ ) {
      if ( i < ( num_of_components - 1 ) ) {
         retval = PAPI_read( _local_components[i].EventSet, _local_components[i].values);
      } else {
         /* get cycles for last component */
         retval = PAPI_read_ts( _local_components[i].EventSet, _local_components[i].values, &_local_cycles );
      }
      HLDBG("Thread-ID:%lu, Component-ID:%d\n", PAPI_thread_id(), components[i].component_id);
      for ( j = 0; j < components[i].num_of_events; j++ ) {
        HLDBG("Thread-ID:%lu, %s:%lld\n", PAPI_thread_id(), components[i].event_names[j], _local_components[i].values[j]);
      }

      if ( retval != PAPI_OK )
         return ( retval );
   }
   return ( PAPI_OK );
}

static int _internal_hl_read_and_store_counters( const char *region, enum region_type reg_typ )
{
   int retval;
   /* read all events */
   if ( ( retval = _internal_hl_read_counters() ) != PAPI_OK ) {
      verbose_fprintf(stdout, "PAPI-HL Error: Could not read counters for thread %lu.\n", PAPI_thread_id());
      _internal_hl_clean_up_all(true);
      return ( retval );
   }

   /* store all events */
   if ( ( retval = _internal_hl_store_counters( PAPI_thread_id(), region, reg_typ) ) != PAPI_OK ) {
      verbose_fprintf(stdout, "PAPI-HL Error: Could not store counters for thread %lu.\n", PAPI_thread_id());
      verbose_fprintf(stdout, "PAPI-HL Advice: Check if your regions are matching.\n");
      _internal_hl_clean_up_all(true);
      return ( retval );
   }
   return ( PAPI_OK );
}

static int _internal_hl_create_global_binary_tree()
{
   if ( ( binary_tree = (binary_tree_t*)malloc(sizeof(binary_tree_t)) ) == NULL )
      return ( PAPI_ENOMEM );
   binary_tree->root = NULL;
   if ( ( binary_tree->find_p = (threads_t*)malloc(sizeof(threads_t)) ) == NULL )
      return ( PAPI_ENOMEM );
   return ( PAPI_OK );
}


static int _internal_hl_mkdir(const char *dir)
{
   int retval;
   int errno;
   char *tmp = NULL;
   char *p = NULL;
   size_t len;

   if ( ( tmp = strdup(dir) ) == NULL )
      return ( PAPI_ENOMEM );
   len = strlen(tmp);

   if(tmp[len - 1] == '/')
      tmp[len - 1] = 0;
   for(p = tmp + 1; *p; p++)
   {
      if(*p == '/')
      {
         *p = 0;
         errno = 0;
         retval = mkdir(tmp, S_IRWXU);
         if ( retval != 0 && errno != EEXIST )
            return ( PAPI_ESYS );
         *p = '/';
      }
   }
   retval = mkdir(tmp, S_IRWXU);
   if ( retval != 0 && errno != EEXIST )
      return ( PAPI_ESYS );
   free(tmp);

   return ( PAPI_OK );
}

static int _internal_hl_determine_output_path()
{
   /* check if PAPI_OUTPUT_DIRECTORY is set */
   char *output_prefix = NULL;
   if ( getenv("PAPI_OUTPUT_DIRECTORY") != NULL ) {
      if ( ( output_prefix = strdup( getenv("PAPI_OUTPUT_DIRECTORY") ) ) == NULL )
         return ( PAPI_ENOMEM );
   } else {
      if ( ( output_prefix = strdup( getcwd(NULL,0) ) ) == NULL )
         return ( PAPI_ENOMEM );
   }
   
   /* generate absolute path for measurement directory */
   if ( ( absolute_output_file_path = (char *)malloc((strlen(output_prefix) + 64) * sizeof(char)) ) == NULL )
      return ( PAPI_ENOMEM );
   if ( output_counter > 0 )
      sprintf(absolute_output_file_path, "%s/papi_%d", output_prefix, output_counter);
   else
      sprintf(absolute_output_file_path, "%s/papi", output_prefix);

   /* check if directory already exists */
   struct stat buf;
   if ( stat(absolute_output_file_path, &buf) == 0 && S_ISDIR(buf.st_mode) ) {

      /* rename old directory by adding a timestamp */
      char *new_absolute_output_file_path = NULL;
      if ( ( new_absolute_output_file_path = (char *)malloc((strlen(absolute_output_file_path) + 64) * sizeof(char)) ) == NULL )
         return ( PAPI_ENOMEM );

      /* create timestamp */
      time_t t = time(NULL);
      struct tm tm = *localtime(&t);
      char m_time[32];
      sprintf(m_time, "%d%02d%02d-%02d%02d%02d", tm.tm_year+1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
      /* add timestamp to existing folder string */
      sprintf(new_absolute_output_file_path, "%s-%s", absolute_output_file_path, m_time);

      uintmax_t current_unix_time = (uintmax_t)t;
      uintmax_t unix_time_from_old_directory = buf.st_mtime;

      /* This is a workaround for MPI applications!!!
       * Only rename existing measurement directory when it is older than
       * current timestamp. If it's not, we assume that another MPI process already created a new measurement directory. */
      if ( unix_time_from_old_directory < current_unix_time ) {

         if ( rename(absolute_output_file_path, new_absolute_output_file_path) != 0 ) {
            verbose_fprintf(stdout, "PAPI-HL Warning: Cannot rename old measurement directory.\n");
            verbose_fprintf(stdout, "If you use MPI, another process may have already renamed the directory.\n");
         }
      }

      free(new_absolute_output_file_path);
   }
   free(output_prefix);
   output_counter++;

   return ( PAPI_OK );
}

static void _internal_hl_json_line_break_and_indent( FILE* f, bool b, int width )
{
   int i;
   if ( b ) {
      fprintf(f, "\n");
      for ( i = 0; i < width; ++i )
         fprintf(f, "  ");
   }
}

static void _internal_hl_json_region_events(FILE* f, bool beautifier, regions_t *regions)
{
   char **all_event_names = NULL;
   int extended_total_num_events;
   int i, j, cmp_iter;

   /* generate array of all events including region count and CPU cycles for output */
   extended_total_num_events = total_num_events + 2;
   all_event_names = (char**)malloc(extended_total_num_events * sizeof(char*));
   all_event_names[0] = "region_count";
   all_event_names[1] = "cycles";
   cmp_iter = 2;
   for ( i = 0; i < num_of_components; i++ ) {
      for ( j = 0; j < components[i].num_of_events; j++ ) {
         all_event_names[cmp_iter++] = components[i].event_names[j];
      }
   }

   for ( j = 0; j < extended_total_num_events; j++ ) {

      _internal_hl_json_line_break_and_indent(f, beautifier, 6);

      /* print read values if available */
      if ( regions->values[j].read_values != NULL) {
         reads_t* read_node = regions->values[j].read_values;
         /* going to last node */
         while ( read_node->next != NULL ) {
            read_node = read_node->next;
         }
         /* read values in reverse order */
         int read_cnt = 1;
         fprintf(f, "\"%s\":{", all_event_names[j]);

         _internal_hl_json_line_break_and_indent(f, beautifier, 7);
         fprintf(f, "\"total\":\"%lld\",", regions->values[j].total);

         while ( read_node != NULL ) {
            _internal_hl_json_line_break_and_indent(f, beautifier, 7);
            fprintf(f, "\"read_%d\":\"%lld\"", read_cnt,read_node->value);

            read_node = read_node->prev;

            if ( read_node == NULL ) {
               _internal_hl_json_line_break_and_indent(f, beautifier, 6);
               fprintf(f, "}");
               if ( j < extended_total_num_events - 1 )
                  fprintf(f, ",");
            } else {
               fprintf(f, ",");
            }

            read_cnt++;
         }
      } else {
         HLDBG("  %s:%lld\n", all_event_names[j], regions->values[j].total);

         if ( j == ( extended_total_num_events - 1 ) ) {
            fprintf(f, "\"%s\":\"%lld\"", all_event_names[j], regions->values[j].total);
         } else {
            fprintf(f, "\"%s\":\"%lld\",", all_event_names[j], regions->values[j].total);
         }
      }
   }

   free(all_event_names);
}

static void _internal_hl_json_regions(FILE* f, bool beautifier, threads_t* thread_node)
{
   /* iterate over regions list */
   regions_t *regions = thread_node->value;

   /* going to last node */
   while ( regions->next != NULL ) {
      regions = regions->next;
   }

   /* read regions in reverse order */
   while (regions != NULL) {
      HLDBG("  Region:%s\n", regions->region);

      _internal_hl_json_line_break_and_indent(f, beautifier, 4);
      fprintf(f, "{");
      _internal_hl_json_line_break_and_indent(f, beautifier, 5);
      fprintf(f, "\"%s\":{", regions->region);

      _internal_hl_json_region_events(f, beautifier, regions);

      _internal_hl_json_line_break_and_indent(f, beautifier, 5);
      fprintf(f, "}");

      regions = regions->prev;
      _internal_hl_json_line_break_and_indent(f, beautifier, 4);
      if (regions == NULL ) {
         fprintf(f, "}");
      } else {
         fprintf(f, "},");
      }
   }
}

static void _internal_hl_json_threads(FILE* f, bool beautifier, unsigned long* tids, int threads_num)
{
   int i;

   _internal_hl_json_line_break_and_indent(f, beautifier, 1);
   fprintf(f, "\"threads\":[");

   /* get regions of all threads */
   for ( i = 0; i < threads_num; i++ )
   {
      HLDBG("Thread ID:%lu\n", tids[i]);
      /* find values of current thread in global binary tree */
      threads_t* thread_node = _internal_hl_find_thread_node(tids[i]);
      if ( thread_node != NULL ) {
         /* do we really need the exact thread id? */
         _internal_hl_json_line_break_and_indent(f, beautifier, 2);
         fprintf(f, "{");
         _internal_hl_json_line_break_and_indent(f, beautifier, 3);
         fprintf(f, "\"id\":\"%lu\",", thread_node->key);

         /* in case we only store iterator id as thread id */
         //fprintf(f, "\"ID\":%d,", i);

         _internal_hl_json_line_break_and_indent(f, beautifier, 3);
         fprintf(f, "\"regions\":[");

         _internal_hl_json_regions(f, beautifier, thread_node);

         _internal_hl_json_line_break_and_indent(f, beautifier, 3);
         fprintf(f, "]");

         _internal_hl_json_line_break_and_indent(f, beautifier, 2);
         if ( i < threads_num - 1 ) {
            fprintf(f, "},");
         } else {
            fprintf(f, "}");
         }
      }
   }

   _internal_hl_json_line_break_and_indent(f, beautifier, 1);
   fprintf(f, "]");
}

static void _internal_hl_write_output()
{
   if ( output_generated == false )
   {
      _papi_hwi_lock( HIGHLEVEL_LOCK );
      if ( output_generated == false ) {
         /* check if events were recorded */
         if ( binary_tree == NULL ) {
            verbose_fprintf(stdout, "PAPI-HL Info: No events were recorded.\n");
            return;
         }
         unsigned long *tids = NULL;
         int number_of_threads;
         FILE *output_file;
         /* current CPU frequency in MHz */
         int cpu_freq;

         if ( region_begin_cnt == region_end_cnt ) {
            verbose_fprintf(stdout, "PAPI-HL Info: Print results...\n");
         } else {
            verbose_fprintf(stdout, "PAPI-HL Warning: Cannot generate output due to not matching regions.\n");
            output_generated = true;
            HLDBG("region_begin_cnt=%d, region_end_cnt=%d\n", region_begin_cnt, region_end_cnt);
            _papi_hwi_unlock( HIGHLEVEL_LOCK );
            return;
         }

         /* create new measurement directory */
         if ( ( _internal_hl_mkdir(absolute_output_file_path) ) != PAPI_OK ) {
            verbose_fprintf(stdout, "PAPI-HL Error: Cannot create measurement directory %s.\n", absolute_output_file_path);
            return;
         }

         /* determine rank for output file */
         int rank = _internal_hl_determine_rank();

         if ( rank < 0 )
         {
            /* generate unique rank number */
            sprintf(absolute_output_file_path + strlen(absolute_output_file_path), "/rank_XXXXXX");
            int fd;
            fd = mkstemp(absolute_output_file_path);
            close(fd);
         }
         else
         {
            sprintf(absolute_output_file_path + strlen(absolute_output_file_path), "/rank_%04d", rank);
         }

         /* determine current cpu frequency */
         cpu_freq = PAPI_get_opt( PAPI_CLOCKRATE, NULL );

         output_file = fopen(absolute_output_file_path, "w");

         if ( output_file == NULL )
         {
            verbose_fprintf(stdout, "PAPI-HL Error: Cannot create output file %s!\n", absolute_output_file_path);
            return;
         }
         else
         {
            /* list all threads */
            if ( PAPI_list_threads( tids, &number_of_threads ) != PAPI_OK ) {
               verbose_fprintf(stdout, "PAPI-HL Error: PAPI_list_threads call failed!\n");
               return;
            }
            if ( ( tids = malloc( number_of_threads * sizeof(unsigned long) ) ) == NULL ) {
               verbose_fprintf(stdout, "PAPI-HL Error: OOM!\n");
               return;
            }
            if ( PAPI_list_threads( tids, &number_of_threads ) != PAPI_OK ) {
               verbose_fprintf(stdout, "PAPI-HL Error: PAPI_list_threads call failed!\n");
               return;
            }

            /* start writing json file */

            /* JSON beautifier (line break and indent) */
            bool beautifier = true;

            /* start of JSON file */
            fprintf(output_file, "{");
            _internal_hl_json_line_break_and_indent(output_file, beautifier, 1);
            fprintf(output_file, "\"cpu in mhz\":\"%d\",", cpu_freq);

            /* write all regions with events per thread */
            _internal_hl_json_threads(output_file, beautifier, tids, number_of_threads);

            /* end of JSON file */
            _internal_hl_json_line_break_and_indent(output_file, beautifier, 0);
            fprintf(output_file, "}");
            fprintf(output_file, "\n");

            fclose(output_file);
            free(tids);

            if ( getenv("PAPI_REPORT") != NULL ) {
               /* print output to stdout */
               printf("\n\nPAPI-HL Output:\n");
               output_file = fopen(absolute_output_file_path, "r");
               int c = fgetc(output_file); 
               while (c != EOF)
               {
                  printf("%c", c);
                  c = fgetc(output_file);
               }
               printf("\n");
               fclose(output_file);
            }

         }

         output_generated = true;
      }
      _papi_hwi_unlock( HIGHLEVEL_LOCK );
   }
}

static void _internal_hl_clean_up_local_data()
{
   int i, retval;
   /* destroy all EventSets from local data */
   if ( _local_components != NULL ) {
      HLDBG("Thread-ID:%lu\n", PAPI_thread_id());
      for ( i = 0; i < num_of_components; i++ ) {
         if ( ( retval = PAPI_stop( _local_components[i].EventSet, _local_components[i].values ) ) != PAPI_OK )
            /* only print error when event set is running */
            if ( retval != -9 )
              verbose_fprintf(stdout, "PAPI-HL Error: PAPI_stop failed: %d.\n", retval);
         if ( ( retval = PAPI_cleanup_eventset (_local_components[i].EventSet) ) != PAPI_OK )
            verbose_fprintf(stdout, "PAPI-HL Error: PAPI_cleanup_eventset failed: %d.\n", retval);
         if ( ( retval = PAPI_destroy_eventset (&_local_components[i].EventSet) ) != PAPI_OK )
            verbose_fprintf(stdout, "PAPI-HL Error: PAPI_destroy_eventset failed: %d.\n", retval);
         free(_local_components[i].values);
      }
      free(_local_components);
      _local_components = NULL;

      /* count global thread variable */
      _papi_hwi_lock( HIGHLEVEL_LOCK );
      num_of_cleaned_threads++;
      _papi_hwi_unlock( HIGHLEVEL_LOCK );
   }
   _local_state = PAPIHL_DEACTIVATED;
}

static void _internal_hl_clean_up_global_data()
{
   int i;
   int extended_total_num_events;

   /* clean up binary tree of recorded events */
   threads_t *thread_node;
   if ( binary_tree != NULL ) {
      while ( binary_tree->root != NULL ) {
         thread_node = *(threads_t **)binary_tree->root;

         /* clean up double linked list of region data */
         regions_t *region = thread_node->value;
         regions_t *tmp;
         while ( region != NULL ) {

            /* clean up read node list */
            extended_total_num_events = total_num_events + 2;
            for ( i = 0; i < extended_total_num_events; i++ ) {
               reads_t *read_node = region->values[i].read_values;
               reads_t *read_node_tmp;
               while ( read_node != NULL ) {
                  read_node_tmp = read_node;
                  read_node = read_node->next;
                  free(read_node_tmp);
               }
            }

            tmp = region;
            region = region->next;

            free(tmp->region);
            free(tmp);
         }
         free(region);

         tdelete(thread_node, &binary_tree->root, compar);
         free(thread_node);
      }
   }

   /* we cannot free components here since other threads could still use them */

   /* clean up requested event names */
   for ( i = 0; i < num_of_requested_events; i++ )
      free(requested_event_names[i]);
   free(requested_event_names);

   free(absolute_output_file_path);
}

static void _internal_hl_clean_up_all(bool deactivate)
{
   int i, num_of_threads;

   /* we assume that output has been already generated or
    * cannot be generated due to previous errors */
   output_generated = true;

   /* clean up thread local data */
   if ( _local_state == PAPIHL_ACTIVE ) {
     HLDBG("Clean up thread local data for thread %lu\n", PAPI_thread_id());
     _internal_hl_clean_up_local_data();
   }

   /* clean up global data */
   if ( state == PAPIHL_ACTIVE ) {
      _papi_hwi_lock( HIGHLEVEL_LOCK );
      if ( state == PAPIHL_ACTIVE ) {

         verbose_fprintf(stdout, "PAPI-HL Info: Output generation is deactivated!\n");

         HLDBG("Clean up global data for thread %lu\n", PAPI_thread_id());
         _internal_hl_clean_up_global_data();

         /* check if all other registered threads have cleaned up */
         PAPI_list_threads(NULL, &num_of_threads);

         HLDBG("Number of registered threads: %d.\n", num_of_threads);
         HLDBG("Number of cleaned threads: %d.\n", num_of_cleaned_threads);

         if ( _internal_hl_check_for_clean_thread_states() == PAPI_OK &&
               num_of_threads == num_of_cleaned_threads ) {
            PAPI_shutdown();
            /* clean up components */
            for ( i = 0; i < num_of_components; i++ ) {
               free(components[i].event_names);
               free(components[i].event_codes);
               free(components[i].event_types);
            }
            free(components);
            HLDBG("PAPI-HL shutdown!\n");
         } else {
            verbose_fprintf(stdout, "PAPI-HL Warning: Could not call PAPI_shutdown() since some threads still have running event sets. Make sure to call PAPI_hl_cleanup_thread() at the end of all parallel regions and PAPI_hl_finalize() in the master thread!\n");
         }

         /* deactivate PAPI-HL */
         if ( deactivate )
            state = PAPIHL_DEACTIVATED;
      }
      _papi_hwi_unlock( HIGHLEVEL_LOCK );
   }
}

static int _internal_hl_check_for_clean_thread_states()
{
   EventSetInfo_t *ESI;
   DynamicArray_t *map = &_papi_hwi_system_info.global_eventset_map;
   int i;

   for( i = 0; i < map->totalSlots; i++ ) {
      ESI = map->dataSlotArray[i];
      if ( ESI ) {
         if ( ESI->state & PAPI_RUNNING ) 
            return ( PAPI_EISRUN );
      }
   }
   return ( PAPI_OK );
}

/** @class PAPI_hl_init
 * @brief Initializes the high-level PAPI library.
 *
 * @par C Interface:
 * \#include <papi.h> @n
 * int PAPI_hl_init();
 *
 * @retval PAPI_OK 
 * @retval PAPI_HIGH_LEVEL_INITED 
 * -- Initialization was already called.
 * @retval PAPI_EMISC
 * -- Initialization failed.
 * @retval PAPI_ENOMEM
 * -- Insufficient memory.
 *
 * PAPI_hl_init initializes the PAPI library and some high-level specific features.
 * If your application is making use of threads you do not need to call any other low level
 * initialization functions as PAPI_hl_init includes thread support.
 * Note that the first call of PAPI_hl_region_begin will automatically call PAPI_hl_init
 * if not already called.
 *
 * @par Example:
 *
 * @code
 * int retval;
 *
 * retval = PAPI_hl_init();
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 * @endcode
 *
 * @see PAPI_hl_cleanup_thread
 * @see PAPI_hl_finalize
 * @see PAPI_hl_set_events
 * @see PAPI_hl_region_begin
 * @see PAPI_hl_read
 * @see PAPI_hl_region_end
 * @see PAPI_hl_print_output
 */
int
_internal_PAPI_hl_init()
{
   if ( state == PAPIHL_ACTIVE ) {
      if ( hl_initiated == false && hl_finalized == false ) {
         _internal_hl_onetime_library_init();
         /* check if the library has been initialized successfully */
         if ( state == PAPIHL_DEACTIVATED )
            return ( PAPI_EMISC );
         return ( PAPI_OK );
      }
      return ( PAPI_ENOINIT );
   }
   return ( PAPI_EMISC );
}

/** @class PAPI_hl_cleanup_thread
 * @brief Cleans up all thread-local data.
 * 
 * @par C Interface:
 * \#include <papi.h> @n
 * void PAPI_hl_cleanup_thread( );
 *
 * @retval PAPI_OK
 * @retval PAPI_EMISC
 * -- Thread has been already cleaned up or PAPI is deactivated due to previous errors.
 * 
 * PAPI_hl_cleanup_thread shuts down thread-local event sets and cleans local
 * data structures. It is recommended to use this function in combination with
 * PAPI_hl_finalize if your application is making use of threads.
 *
 * @par Example:
 *
 * @code
 * int retval;
 *
 * #pragma omp parallel
 * {
 *   retval = PAPI_hl_region_begin("computation");
 *   if ( retval != PAPI_OK )
 *       handle_error(1);
 *
 *    //Do some computation here
 *
 *   retval = PAPI_hl_region_end("computation");
 *   if ( retval != PAPI_OK )
 *       handle_error(1);
 * 
 *   retval = PAPI_hl_cleanup_thread();
 *   if ( retval != PAPI_OK )
 *       handle_error(1);
 * }
 *
 * retval = PAPI_hl_finalize();
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 * @endcode
 *
 * @see PAPI_hl_init
 * @see PAPI_hl_finalize
 * @see PAPI_hl_set_events
 * @see PAPI_hl_region_begin
 * @see PAPI_hl_read
 * @see PAPI_hl_region_end
 * @see PAPI_hl_print_output
 */
int _internal_PAPI_hl_cleanup_thread()
{
   if ( state == PAPIHL_ACTIVE && 
        hl_initiated == true && 
        _local_state == PAPIHL_ACTIVE ) {
         /* do not clean local data from master thread */
         if ( master_thread_id != PAPI_thread_id() )
           _internal_hl_clean_up_local_data();
         return ( PAPI_OK );
      }
   return ( PAPI_EMISC );
}

/** @class PAPI_hl_finalize
 * @brief Finalizes the high-level PAPI library.
 *
 * @par C Interface:
 * \#include <papi.h> @n
 * int PAPI_hl_finalize( );
 *
 * @retval PAPI_OK
 * @retval PAPI_EMISC
 * -- PAPI has been already finalized or deactivated due to previous errors.
 *
 * PAPI_hl_finalize finalizes the high-level library by destroying all counting event sets
 * and internal data structures.
 *
 * @par Example:
 *
 * @code
 * int retval;
 *
 * retval = PAPI_hl_finalize();
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 * @endcode
 *
 * @see PAPI_hl_init
 * @see PAPI_hl_cleanup_thread
 * @see PAPI_hl_set_events
 * @see PAPI_hl_region_begin
 * @see PAPI_hl_read
 * @see PAPI_hl_region_end
 * @see PAPI_hl_print_output
 */
int _internal_PAPI_hl_finalize()
{
   if ( state == PAPIHL_ACTIVE && hl_initiated == true ) {
      _internal_hl_clean_up_all(true);
      return ( PAPI_OK );
   }
   return ( PAPI_EMISC );
}

/** @class PAPI_hl_set_events
 * @brief Generates event sets based on a list of hardware events.
 *
 * @par C Interface:
 * \#include <papi.h> @n
 * int PAPI_hl_set_events( const char* events );
 *
 * @param events
 * -- list of hardware events separated by commas
 *
 * @retval PAPI_OK
 * @retval PAPI_EMISC
 * -- PAPI has been deactivated due to previous errors.
 * @retval PAPI_ENOMEM
 * -- Insufficient memory.
 *
 * PAPI_hl_set_events offers the user the possibility to determine hardware events in
 * the source code as an alternative to the environment variable PAPI_EVENTS.
 * Note that the content of PAPI_EVENTS is ignored if PAPI_hl_set_events was successfully executed.
 * If the events argument cannot be interpreted, default hardware events are
 * taken for the measurement.
 *
 * @par Example:
 *
 * @code
 * int retval;
 *
 * retval = PAPI_hl_set_events("PAPI_TOT_INS,PAPI_TOT_CYC");
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 * @endcode
 *
 * @see PAPI_hl_init
 * @see PAPI_hl_cleanup_thread
 * @see PAPI_hl_finalize
 * @see PAPI_hl_region_begin
 * @see PAPI_hl_read
 * @see PAPI_hl_region_end
 * @see PAPI_hl_print_output
 */
int
_internal_PAPI_hl_set_events(const char* events)
{
   int retval;
   if ( state == PAPIHL_ACTIVE ) {

      /* This may only be called once after the high-level API was successfully
       * initiated. Any second call just returns PAPI_OK without doing an
       * expensive lock. */
      if ( hl_initiated == true ) {
         if ( events_determined == false )
         {
            _papi_hwi_lock( HIGHLEVEL_LOCK );
            if ( events_determined == false && state == PAPIHL_ACTIVE )
            {
               HLDBG("Set events: %s\n", events);
               if ( ( retval = _internal_hl_read_events(events) ) != PAPI_OK ) {
                  state = PAPIHL_DEACTIVATED;
                  _internal_hl_clean_up_global_data();
                  _papi_hwi_unlock( HIGHLEVEL_LOCK );
                  return ( retval );
               }
               if ( ( retval = _internal_hl_create_global_binary_tree() ) != PAPI_OK ) {
                  state = PAPIHL_DEACTIVATED;
                  _internal_hl_clean_up_global_data();
                  _papi_hwi_unlock( HIGHLEVEL_LOCK );
                  return ( retval );
               }
            }
            _papi_hwi_unlock( HIGHLEVEL_LOCK );
         }
      }
      /* in case the first locked thread ran into problems */
      if ( state == PAPIHL_DEACTIVATED)
         return ( PAPI_EMISC );
      return ( PAPI_OK );
   }
   return ( PAPI_EMISC );
}

/** @class PAPI_hl_print_output
 * @brief Prints values of hardware events.
 *
 * @par C Interface:
 * \#include <papi.h> @n
 * void PAPI_hl_print_output( );
 *
 * PAPI_hl_print_output prints the measured values of hardware events in one file for serial
 * or thread parallel applications.
 * Multi-processing applications, such as MPI, will have one output file per process.
 * Each output file contains measured values of all threads.
 * The entire measurement can be converted in a better readable output via python.
 * For more information, see <a href="https://bitbucket.org/icl/papi/wiki/papi-hl.md">High Level API</a>.
 * Note that if PAPI_hl_print_output is not called explicitly PAPI will try to generate output
 * at the end of the application. However, for some reason, this feature sometimes does not  work.
 * It is therefore recommended to call PAPI_hl_print_output for larger applications.
 *
 * @par Example:
 *
 * @code
 *
 * PAPI_hl_print_output();
 *
 * @endcode
 *
 * @see PAPI_hl_init
 * @see PAPI_hl_cleanup_thread
 * @see PAPI_hl_finalize
 * @see PAPI_hl_set_events
 * @see PAPI_hl_region_begin
 * @see PAPI_hl_read
 * @see PAPI_hl_region_end 
 */
void
_internal_PAPI_hl_print_output()
{
   if ( state == PAPIHL_ACTIVE && 
        hl_initiated == true && 
        output_generated == false ) {
      _internal_hl_write_output();
   }
}

/** @class PAPI_hl_region_begin
 * @brief Reads and stores hardware events at the beginning of an instrumented code region.
 *
 * @par C Interface:
 * \#include <papi.h> @n
 * int PAPI_hl_region_begin( const char* region );
 *
 * @param region
 * -- a unique region name
 *
 * @retval PAPI_OK
 * @retval PAPI_ENOTRUN
 * -- EventSet is currently not running or could not determined.
 * @retval PAPI_ESYS
 * -- A system or C library call failed inside PAPI, see the errno variable.
 * @retval PAPI_EMISC
 * -- PAPI has been deactivated due to previous errors.
 * @retval PAPI_ENOMEM
 * -- Insufficient memory.
 *
 * PAPI_hl_region_begin reads hardware events and stores them internally at the beginning
 * of an instrumented code region.
 * If not specified via environment variable PAPI_EVENTS, default events are used.
 * The first call sets all counters implicitly to zero and starts counting.
 * Note that if PAPI_EVENTS is not set or cannot be interpreted, default hardware events are
 * recorded.
 *
 * @par Example:
 *
 * @code
 * export PAPI_EVENTS="PAPI_TOT_INS,PAPI_TOT_CYC"
 * @endcode
 *
 *
 * @code
 * int retval;
 *
 * retval = PAPI_hl_region_begin("computation");
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 *  //Do some computation here
 *
 * retval = PAPI_hl_region_end("computation");
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 * @endcode
 *
 * @see PAPI_hl_read
 * @see PAPI_hl_region_end
 */
int
PAPI_hl_region_begin( const char* region )
{
   int retval;

   if ( state == PAPIHL_DEACTIVATED ) {
      /* check if we have to clean up local stuff */
      if ( _local_state == PAPIHL_ACTIVE )
         _internal_hl_clean_up_local_data();
      return ( PAPI_EMISC );
   }

   if ( hl_finalized == true )
      return ( PAPI_ENOTRUN );

   if ( hl_initiated == false ) {
      if ( ( retval = _internal_PAPI_hl_init() ) != PAPI_OK )
         return ( retval );
   }

   if ( events_determined == false ) {
      if ( ( retval = _internal_PAPI_hl_set_events(NULL) ) != PAPI_OK )
         return ( retval );
   }

   if ( _local_components == NULL ) {
      if ( ( retval = _internal_hl_create_event_sets() ) != PAPI_OK ) {
         HLDBG("Could not create local events sets for thread %lu.\n", PAPI_thread_id());
         _internal_hl_clean_up_all(true);
         return ( retval );
      }
   }

   /* read and store all events */
   HLDBG("Thread ID:%lu, Region:%s\n", PAPI_thread_id(), region);
   if ( ( retval = _internal_hl_read_and_store_counters(region, REGION_BEGIN) ) != PAPI_OK )
      return ( retval );

   _local_region_begin_cnt++;
   return ( PAPI_OK );
}

/** @class PAPI_hl_read
 * @brief Reads and stores hardware events inside of an instrumented code region.
 *
 * @par C Interface:
 * \#include <papi.h> @n
 * int PAPI_hl_read( const char* region );
 *
 * @param region
 * -- a unique region name corresponding to PAPI_hl_region_begin
 *
 * @retval PAPI_OK
 * @retval PAPI_ENOTRUN
 * -- EventSet is currently not running or could not determined.
 * @retval PAPI_ESYS
 * -- A system or C library call failed inside PAPI, see the errno variable.
 * @retval PAPI_EMISC
 * -- PAPI has been deactivated due to previous errors.
 * @retval PAPI_ENOMEM
 * -- Insufficient memory.
 *
 * PAPI_hl_read reads hardware events and stores them internally inside
 * of an instrumented code region.
 * Assumes that PAPI_hl_region_begin was called before.
 *
 * @par Example:
 *
 * @code
 * int retval;
 *
 * retval = PAPI_hl_region_begin("computation");
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 *  //Do some computation here
 *
 * retval = PAPI_hl_read("computation");
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 *  //Do some computation here
 *
 * retval = PAPI_hl_region_end("computation");
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 * @endcode
 *
 * @see PAPI_hl_region_begin
 * @see PAPI_hl_region_end
 */
int
PAPI_hl_read(const char* region)
{
   int retval;

   if ( state == PAPIHL_DEACTIVATED ) {
      /* check if we have to clean up local stuff */
      if ( _local_state == PAPIHL_ACTIVE )
         _internal_hl_clean_up_local_data();
      return ( PAPI_EMISC );
   }

   if ( _local_region_begin_cnt == 0 ) {
      verbose_fprintf(stdout, "PAPI-HL Warning: Cannot find matching region for PAPI_hl_read(\"%s\") for thread %lu.\n", region, PAPI_thread_id());
      return ( PAPI_EMISC );
   }

   if ( _local_components == NULL )
      return ( PAPI_ENOTRUN );

   /* read and store all events */
   HLDBG("Thread ID:%lu, Region:%s\n", PAPI_thread_id(), region);
   if ( ( retval = _internal_hl_read_and_store_counters(region, REGION_READ) ) != PAPI_OK )
      return ( retval );

   return ( PAPI_OK );
}

/** @class PAPI_hl_region_end
 * @brief Reads and stores hardware events at the end of an instrumented code region.
 *
 * @par C Interface:
 * \#include <papi.h> @n
 * int PAPI_hl_region_end( const char* region );
 *
 * @param region
 * -- a unique region name corresponding to PAPI_hl_region_begin
 *
 * @retval PAPI_OK
 * @retval PAPI_ENOTRUN
 * -- EventSet is currently not running or could not determined.
 * @retval PAPI_ESYS
 * -- A system or C library call failed inside PAPI, see the errno variable.
 * @retval PAPI_EMISC
 * -- PAPI has been deactivated due to previous errors.
 * @retval PAPI_ENOMEM
 * -- Insufficient memory.
 *
 * PAPI_hl_region_end reads hardware events and stores the difference to the values from
 * PAPI_hl_region_begin at the end of an instrumented code region.
 * Assumes that PAPI_hl_region_begin was called before.
 * Note that an output is automatically generated when your application terminates.
 * 
 *
 * @par Example:
 *
 * @code
 * int retval;
 *
 * retval = PAPI_hl_region_begin("computation");
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 *  //Do some computation here
 *
 * retval = PAPI_hl_region_end("computation");
 * if ( retval != PAPI_OK )
 *     handle_error(1);
 *
 * @endcode
 *
 * @see PAPI_hl_region_begin
 * @see PAPI_hl_read
 */
int
PAPI_hl_region_end( const char* region )
{
   int retval;

   if ( state == PAPIHL_DEACTIVATED ) {
      /* check if we have to clean up local stuff */
      if ( _local_state == PAPIHL_ACTIVE )
         _internal_hl_clean_up_local_data();
      return ( PAPI_EMISC );
   }

   if ( _local_region_begin_cnt == 0 ) {
      verbose_fprintf(stdout, "PAPI-HL Warning: Cannot find matching region for PAPI_hl_region_end(\"%s\") for thread %lu.\n", region, PAPI_thread_id());
      return ( PAPI_EMISC );
   }

   if ( _local_components == NULL )
      return ( PAPI_ENOTRUN );

   /* read and store all events */
   HLDBG("Thread ID:%lu, Region:%s\n", PAPI_thread_id(), region);
   if ( ( retval = _internal_hl_read_and_store_counters(region, REGION_END) ) != PAPI_OK )
      return ( retval );

   _local_region_end_cnt++;
   return ( PAPI_OK );
}

