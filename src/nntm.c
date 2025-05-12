

///////////// TODO: Do not forget the mutexes...

/*
 * todo‑viewer.c  – ncurses list with date / priority / text columns
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>  // for open()
#include <libgen.h> // for dirname
#include <locale.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // for fork(), execl(), _exit()

#include <pthread.h>
#include <sys/stat.h> // for fstat(), S_ISREG, S_ISFIFO, for streaming by pipe functionality

#define MAX_TODOS 1000
#define MAX_LINE 512
#define MAX_TYPE 32

typedef struct
{
      bool completed;
      char completion_date[ 11 ]; /* YYYY‑MM‑DD               */
      char date[ 11 ];            /* due date / log date      */
      char priority[ 4 ];         /* "(A)" .. "(Z)" or ""     */
      char type[ MAX_TYPE ];      /* @context / @project      */
      char text[ MAX_LINE ];      /* whatever is left         */
} Todo;

static Todo todos[ MAX_TODOS ];
static int todo_count = 0;

static char *types[ MAX_TODOS ];
static int type_count     = 0;
static int selected_type  = 0;
static int selected_index = 0;

static bool show_help    = false;
static int scroll_offset = 0;

static const char *todo_filename = NULL;

static bool sort_descending      = false;
static bool sort_date_descending = false;

static void save_todos_to_file( void );

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

char archive_path[ PATH_MAX ];

static const char *exec_script = NULL;

bool auto_scroll_enabled = true;

// For our streaming by pipe functionality, mutex for thread safety
bool streaming_mode        = false;
pthread_mutex_t todo_mutex = PTHREAD_MUTEX_INITIALIZER;

static void run_exec_hook( const char *prefix, const char *text )
{
      if ( !exec_script || !text || strlen( text ) == 0 )
            return;

      pid_t pid = fork();
      if ( pid == 0 )
      {
            // In child process
            // Redirect stdout and stderr to /dev/null
            int devnull = open( "/dev/null", O_WRONLY );
            if ( devnull != -1 )
            {
                  dup2( devnull, STDOUT_FILENO );
                  dup2( devnull, STDERR_FILENO );
                  close( devnull );
            }

            char msg[ MAX_LINE * 2 ];
            snprintf( msg, sizeof( msg ), "%s%s", prefix, text );

            execl( exec_script, exec_script, msg, (char *)NULL );
            _exit( 127 ); // only reached if execl fails
      }
      // Parent continues immediately
}

static void archive_completed_todos( void )
{
      // Derive archive path
      char archive_path[ PATH_MAX ];
      strncpy( archive_path, todo_filename, sizeof( archive_path ) );
      archive_path[ sizeof( archive_path ) - 1 ] = '\0';
      char *dir                                  = dirname( archive_path );

      snprintf( archive_path, sizeof archive_path, "%s/todo.archive.txt", dir );

      FILE *f = fopen( archive_path, "a" );
      if ( !f )
      {
            perror( "archive write" );
            return;
      }

      // Write all completed todos and remove them from the list
      int write_count = 0;
      for ( int i = 0; i < todo_count; )
      {
            Todo *t = &todos[ i ];
            if ( !t->completed )
            {
                  i++;
                  continue;
            }

            fprintf( f, "x %s %s @%s %s\n", t->completion_date, t->date,
                     t->type, t->text );

            // Shift remaining todos left
            for ( int j = i; j < todo_count - 1; ++j )
                  todos[ j ] = todos[ j + 1 ];

            todo_count--;
            write_count++;
      }

      fclose( f );

      if ( !streaming_mode )
            if ( write_count > 0 )
                  save_todos_to_file();
}

static void add_new_todo( void )
{
      if ( streaming_mode )
            return;

      if ( todo_count >= MAX_TODOS )
            return;

      Todo new_todo;
      memset( &new_todo, 0, sizeof( Todo ) );

      // Set today's date
      time_t now    = time( NULL );
      struct tm *tm = localtime( &now );
      strftime( new_todo.date, sizeof new_todo.date, "%Y-%m-%d", tm );

      // Set @type from current context
      strncpy( new_todo.type, types[ selected_type ], MAX_TYPE - 1 );

      // Default to not completed
      new_todo.completed = false;

      // Prompt for text
      echo();
      curs_set( 1 );
      move( LINES - 1, 0 );
      clrtoeol();
      attron( COLOR_PAIR( 2 ) | A_BOLD );
      printw( "New todo: " );
      attroff( COLOR_PAIR( 2 ) | A_BOLD );
      getnstr( new_todo.text, MAX_LINE - 1 );
      noecho();
      curs_set( 0 );

      if ( strlen( new_todo.text ) == 0 )
            return;

      // Insert new todo right after the currently selected item
      int shown = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( todos[ i ].type, types[ selected_type ] ) != 0 )
                  continue;

            if ( shown == selected_index )
            {
                  for ( int j = todo_count; j > i + 1; --j )
                        todos[ j ] = todos[ j - 1 ];
                  todos[ i + 1 ] = new_todo;
                  todo_count++;
                  run_exec_hook( "Added: ", new_todo.text );
                  save_todos_to_file();
                  selected_index++;
                  return;
            }
            ++shown;
      }

      // fallback if no match: append at end
      todos[ todo_count++ ] = new_todo;
      save_todos_to_file();
      run_exec_hook( "Added: ", new_todo.text );
}

static void group_todos_by_completed( void )
{
      Todo grouped[ MAX_TODOS ];
      int group_count = 0;
      const char *cat = types[ selected_type ];

      // First: uncompleted
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( ( strcmp( cat, "all" ) == 0 || strcmp( t->type, cat ) == 0 ) &&
                 !t->completed )
                  grouped[ group_count++ ] = *t;
      }

      // Then: completed
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( ( strcmp( cat, "all" ) == 0 || strcmp( t->type, cat ) == 0 ) &&
                 t->completed )
                  grouped[ group_count++ ] = *t;
      }

      // Reinsert grouped section
      int j = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( cat, "all" ) == 0 ||
                 strcmp( todos[ i ].type, cat ) == 0 )
                  todos[ i ] = grouped[ j++ ];
      }
}

static int compare_date( const void *a, const void *b )
{
      const Todo *ta = (const Todo *)a;
      const Todo *tb = (const Todo *)b;

      int cmp = strncmp( ta->date, tb->date, 10 );
      return sort_date_descending ? -cmp : cmp;
}
static int compare_priority( const void *a, const void *b )
{
      const Todo *ta = (const Todo *)a;
      const Todo *tb = (const Todo *)b;

      int pa = ( ta->priority[ 0 ] == '(' ) ? ta->priority[ 1 ] : 127;
      int pb = ( tb->priority[ 0 ] == '(' ) ? tb->priority[ 1 ] : 127;

      return sort_descending ? pb - pa : pa - pb;
}

static void sort_todos_by_date( bool descending )
{
      Todo sorted[ MAX_TODOS ];
      int count = 0;

      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( types[ selected_type ], "all" ) == 0 ||
                 strcmp( todos[ i ].type, types[ selected_type ] ) == 0 )
            {
                  sorted[ count++ ] = todos[ i ];
            }
      }

      sort_date_descending = descending;
      qsort( sorted, count, sizeof( Todo ), compare_date );

      int j = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( types[ selected_type ], "all" ) == 0 ||
                 strcmp( todos[ i ].type, types[ selected_type ] ) == 0 )
            {
                  todos[ i ] = sorted[ j++ ];
            }
      }
}

static void sort_todos_by_priority( bool descending )
{
      Todo sorted[ MAX_TODOS ];
      int count = 0;

      // collect matching todos
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( types[ selected_type ], "all" ) == 0 ||
                 strcmp( todos[ i ].type, types[ selected_type ] ) == 0 )
            {
                  sorted[ count++ ] = todos[ i ];
            }
      }

      sort_descending = descending;
      qsort( sorted, count, sizeof( Todo ), compare_priority );

      // reinsert sorted section
      int j = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            if ( strcmp( types[ selected_type ], "all" ) == 0 ||
                 strcmp( todos[ i ].type, types[ selected_type ] ) == 0 )
            {
                  todos[ i ] = sorted[ j++ ];
            }
      }
}

static void prompt_priority( void )
{
      int shown = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( strcmp( types[ selected_type ], "all" ) != 0 &&
                 strcmp( t->type, types[ selected_type ] ) != 0 )
                  continue;

            if ( shown == selected_index )
            {
                  if ( t->completed )
                  {
                        mvprintw( LINES - 1, 0,
                                  "❌ Cannot set priority on completed "
                                  "item." );
                        refresh();
                        napms( 1000 ); // wait 1 second
                        move( LINES - 1, 0 );
                        clrtoeol();
                        refresh();
                        return;
                  }

                  // Prompt user
                  echo();
                  curs_set( 1 );
                  mvprintw( LINES - 1, 0,
                            "Set priority (a-z, or space to clear): " );
                  int ch = getch();
                  noecho();
                  curs_set( 0 );

                  if ( ch == ' ' || ch == KEY_BACKSPACE || ch == 127 )
                  {
                        t->priority[ 0 ] = '\0'; // clear
                  }
                  else if ( isalpha( ch ) )
                  {
                        ch = toupper( ch );
                        snprintf( t->priority, sizeof t->priority, "(%c)", ch );
                  }

                  if ( !streaming_mode )
                        save_todos_to_file();

                  move( LINES - 1, 0 );
                  clrtoeol();
                  refresh();
                  return;
            }

            ++shown;
      }
}

static void add_type( const char *type )
{
      for ( int i = 0; i < type_count; ++i )
            if ( strcmp( types[ i ], type ) == 0 )
                  return;
      types[ type_count++ ] = strdup( type );
}

static int count_visible_items_for_type( const char *type )
{
      if ( strcmp( type, "all" ) == 0 )
            return todo_count;

      int n = 0;
      for ( int i = 0; i < todo_count; ++i )
            if ( strcmp( todos[ i ].type, type ) == 0 )
                  ++n;
      return n;
}

static void prompt_type( void )
{
      int shown = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( strcmp( types[ selected_type ], "all" ) != 0 &&
                 strcmp( t->type, types[ selected_type ] ) != 0 )
                  continue;

            if ( shown == selected_index )
            {
                  // Prompt for new type
                  echo();
                  curs_set( 1 );
                  char input[ MAX_TYPE ] = { 0 };
                  move( LINES - 1, 0 );
                  clrtoeol();
                  attron( COLOR_PAIR( 2 ) | A_BOLD );
                  printw( "Change type to @" );
                  attroff( COLOR_PAIR( 2 ) | A_BOLD );
                  getnstr( input, MAX_TYPE - 1 );
                  noecho();
                  curs_set( 0 );

                  if ( strlen( input ) > 0 )
                  {
                        strncpy( t->type, input, MAX_TYPE - 1 );
                        add_type( input );
                        save_todos_to_file();
                  }

                  move( LINES - 1, 0 );
                  clrtoeol();
                  refresh();
                  return;
            }

            ++shown;
      }
}
/* ─────────────────────────────────────────────── file I/O ── */

void load_todos( const char *filename )
{
      FILE *f = fopen( filename, "r" );
      if ( !f )
      {
            perror( "open" );
            exit( 1 );
      }
      // Clear current todos and types
      // In case we run it again
      todo_count = 0;
      for ( int i = 0; i < type_count; ++i )
      {
            free( types[ i ] );
      }

      type_count = 0;
      // After clearing types and todos, add the virtual type
      types[ type_count++ ] = strdup( "all" );

      char line[ MAX_LINE ];
      while ( fgets( line, sizeof( line ), f ) )
      {
            if ( todo_count >= MAX_TODOS )
                  break;

            // Trim trailing newlines
            line[ strcspn( line, "\r\n" ) ] = '\0';

            Todo *t = &todos[ todo_count ];
            memset( t, 0, sizeof( Todo ) );
            strncpy( t->text, line, MAX_LINE - 1 );
            strcpy( t->date, "2025-05-12" ); // dummy
            strcpy( t->type, "stream" );
            t->completed = false;

            const char *p = line;

            // 1. check if line starts with "x " (completed)
            if ( strncmp( p, "x ", 2 ) == 0 )
            {
                  t->completed = true;
                  p += 2;
                  sscanf( p, "%10s", t->completion_date );
                  p += strlen( t->completion_date );
                  while ( isspace( (unsigned char)*p ) )
                        p++;
            }

            // 2. check for priority first (before date)
            if ( p[ 0 ] == '(' && isalpha( (unsigned char)p[ 1 ] ) &&
                 p[ 2 ] == ')' )
            {
                  strncpy( t->priority, p, 3 );
                  t->priority[ 3 ] = '\0';
                  p += 4;
                  while ( isspace( (unsigned char)*p ) )
                        p++;
            }
            else
            {
                  t->priority[ 0 ] = '\0';
            }

            // 3. extract date
            sscanf( p, "%10s", t->date );
            p += strlen( t->date );
            while ( isspace( (unsigned char)*p ) )
                  p++;

            // 4. if priority wasn't found before, check again after date
            if ( t->priority[ 0 ] == '\0' && p[ 0 ] == '(' &&
                 isalpha( (unsigned char)p[ 1 ] ) && p[ 2 ] == ')' )
            {
                  strncpy( t->priority, p, 3 );
                  t->priority[ 3 ] = '\0';
                  p += 4;
                  while ( isspace( (unsigned char)*p ) )
                        p++;
            }

            // 5. extract @type
            if ( *p == '@' )
            {
                  ++p;
                  sscanf( p, "%31s", t->type );
                  add_type( t->type );
                  p += strlen( t->type );
                  while ( isspace( (unsigned char)*p ) )
                        p++;
            }
            else
            {
                  strcpy( t->type, "all" );
                  add_type( "all" );
            }

            // 6. remaining is the text
            strncpy( t->text, p, MAX_LINE - 1 );
            todo_count++;
      }

      fclose( f );
}

static void save_todos_to_file( void )
{
      pthread_mutex_lock( &todo_mutex );
      FILE *f = fopen( todo_filename, "w" );
      if ( !f )
      {
            perror( "write" );
            pthread_mutex_unlock( &todo_mutex );
            return;
      }

      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];

            if ( t->completed )
            {
                  // Save completed format:
                  // x <completion_date> <original_date> @type text
                  // [pri:X]
                  fprintf( f, "x %s %s @%s %s", t->completion_date, t->date,
                           t->type, t->text );
            }
            else
            {
                  // Save incomplete format:
                  // (X) <date> @type text
                  if ( t->priority[ 0 ] != '\0' )
                        fprintf( f, "%s %s @%s %s", t->priority, t->date,
                                 t->type, t->text );
                  else
                        fprintf( f, "%s @%s %s", t->date, t->type, t->text );
            }

            fputc( '\n', f );
      }

      fclose( f );
      pthread_mutex_unlock( &todo_mutex );
}

/* ───────────────────────────────────────────── logic ── */
static void toggle_completed( int visible_index )
{
      int shown = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( strcmp( types[ selected_type ], "all" ) != 0 &&
                 strcmp( t->type, types[ selected_type ] ) != 0 )
                  continue;

            if ( shown == visible_index )
            {
                  t->completed = !t->completed;

                  if ( t->completed )
                  {
                        // Set today's date
                        time_t now        = time( NULL );
                        struct tm *tm_now = localtime( &now );
                        strftime( t->completion_date, sizeof t->completion_date,
                                  "%Y-%m-%d", tm_now );

                        // If priority exists, move it to end of text as
                        // "pri:X"
                        if ( t->priority[ 0 ] == '(' &&
                             t->priority[ 2 ] == ')' )
                        {
                              char pri_tag[ 8 ];
                              snprintf( pri_tag, sizeof pri_tag, " pri:%c",
                                        t->priority[ 1 ] );

                              // Only append if not already there
                              if ( !strstr( t->text, pri_tag ) &&
                                   strlen( t->text ) + strlen( pri_tag ) <
                                       MAX_LINE )
                              {
                                    strcat( t->text, pri_tag );
                              }

                              // Clear priority field
                              t->priority[ 0 ] = '\0';
                        }
                        // 🔽 ADD THIS LINE to trigger exec hook
                        run_exec_hook( "Completed: ", t->text );
                  }
                  else
                  {
                        t->completion_date[ 0 ] = '\0';

                        // On un-complete: detect and extract "pri:X"
                        // from end of text
                        char *pri = strstr( t->text, " pri:" );
                        if ( pri && strlen( pri ) == 6 &&
                             isalpha( (unsigned char)pri[ 5 ] ) )
                        {
                              // Restore priority
                              snprintf( t->priority, sizeof t->priority, "(%c)",
                                        pri[ 5 ] );

                              // Remove it from the end of text
                              *pri = '\0';

                              // Also trim trailing whitespace just in
                              // case
                              size_t len = strlen( t->text );
                              while (
                                  len > 0 &&
                                  isspace( (unsigned char)t->text[ len - 1 ] ) )
                              {
                                    t->text[ len - 1 ] = '\0';
                                    len--;
                              }
                        }
                        // 🔽 ADD THIS LINE to trigger exec hook
                        run_exec_hook( "Uncompleted: ", t->text );
                  }

                  if ( !streaming_mode )
                        save_todos_to_file();
                  return;
            }

            ++shown;
      }
}

/* ───────────────────────────────────────────── UI ── */
/* ───────────────────────────────────────────── left type panel ── */
/* Draws the vertical “types” panel and returns its width. */
#define TYPE_PANEL_W 14 /* change once → layout adapts */

static int draw_type_panel( void )
{
      /* border */
      // top line not needed, we have other wider horizonal line. mvprintw( 0,
      // 0, "+-%.*s-+", TYPE_PANEL_W - 2, "--------------" );
      for ( int y = 1; y < LINES - 1; ++y )
            // mvprintw( y, 0, "|%*s|", TYPE_PANEL_W - 2, "" );
            mvprintw( y, 0, " %*s:", TYPE_PANEL_W - 2, "" );
      mvprintw( LINES - 1, 0, "+-%.*s-+", TYPE_PANEL_W - 2, "--------------" );

      /* list every known @type */
      int max_rows = LINES - 2; /* inside the box */
      for ( int i = 0; i < type_count && i < max_rows; ++i )
      {
            bool sel = ( i == selected_type );
            if ( sel )
                  attron( COLOR_PAIR( 2 ) | A_BOLD );
            mvprintw( i + 1, 1, "%-*s", TYPE_PANEL_W - 2, types[ i ] );
            if ( sel )
                  attroff( COLOR_PAIR( 2 ) | A_BOLD );
      }
      return TYPE_PANEL_W;
}

static void draw_ui( void )
{
      erase();

      /* list */
      // -- Side panel
      int panel_w = draw_type_panel(); /* << add this line */

      /* derive column positions from panel_w */
      const int DATE_COL = panel_w + 2;
      const int PRIO_COL = DATE_COL + 11;
      const int TYPE_COL = PRIO_COL + 6;
      const int TEXT_COL = ( strcmp( types[ selected_type ], "all" ) == 0 )
                               ? TYPE_COL + 8
                               : PRIO_COL + 6;
      // ----

      /* help overlay */
      if ( show_help )
      {
            attron( COLOR_PAIR( 2 ) | A_BOLD );
            mvprintw( 0, 0, "HELP — press any key" );
            attroff( COLOR_PAIR( 2 ) | A_BOLD );
            mvprintw( 2, 2, "j/k        move up / down" );
            mvprintw( 3, 2, "h/l        switch context" );
            mvprintw( 4, 2, "SPACE      toggle completed" );
            mvprintw( 5, 2, "?          help" );
            mvprintw( 6, 2, "q          quit" );
            wnoutrefresh( stdscr );
            doupdate();
            return;
      }

      /* header */
      attron( COLOR_PAIR( 2 ) | A_BOLD );
      mvprintw( 0, 0, "   " );
      if ( strcmp( types[ selected_type ], "all" ) == 0 )
      {
            attron( COLOR_PAIR( 9 ) | A_BOLD ); // magenta for 'all'
      }
      else
      {
            attron( COLOR_PAIR( 8 ) | A_BOLD ); // cyan for real contexts
      }
      printw( "@%s", types[ selected_type ] );
      attroff( COLOR_PAIR( 8 ) );
      attroff( COLOR_PAIR( 9 ) );
      attroff( COLOR_PAIR( 2 ) | A_BOLD );

      mvhline( 1, 0, '-', COLS );
      /*
    // --------------- META INFO ----------------
    // Display meta info above horizontal line
    int total     = count_visible_items_for_type( types[ selected_type ] );
    int completed = 0;
    for ( int i = 0; i < todo_count; ++i )
    {
          if ( ( strcmp( types[ selected_type ], "all" ) == 0 ||
                 strcmp( todos[ i ].type, types[ selected_type ] ) == 0 ) &&
               todos[ i ].completed )
          {
                completed++;
          }
    }
    attron( COLOR_PAIR( 5 ) );
    mvprintw( 1, 4, "[%d todos, %d done%s]", total, completed,
              streaming_mode ? ", streaming" : "" );
    attroff( COLOR_PAIR( 5 ) );

    // --------------------------
*/
      int row           = 2;
      int visible_lines = LINES - 2;

      /* ensure scroll_offset keeps selected line on screen */
      if ( selected_index < scroll_offset )
            scroll_offset = selected_index;
      else if ( selected_index >= scroll_offset + visible_lines )
            scroll_offset = selected_index - visible_lines + 1;

      int local_idx = 0;
      for ( int i = 0; i < todo_count; ++i )
      {
            Todo *t = &todos[ i ];
            if ( strcmp( types[ selected_type ], "all" ) != 0 &&
                 strcmp( t->type, types[ selected_type ] ) != 0 )
                  continue;

            if ( local_idx++ < scroll_offset )
                  continue;
            if ( row >= LINES )
                  break;

            bool is_sel = ( local_idx - 1 == selected_index );

            attr_t date_attr, text_attr;
            if ( t->completed )
            {
                  date_attr = is_sel ? ( COLOR_PAIR( 7 ) | A_BOLD )
                                     : ( COLOR_PAIR( 6 ) | A_DIM );
                  text_attr = COLOR_PAIR( 5 ) | ( is_sel ? A_BOLD : A_DIM );
            }
            else
            {
                  date_attr =
                      is_sel ? ( COLOR_PAIR( 4 ) | A_BOLD ) : COLOR_PAIR( 3 );
                  text_attr =
                      is_sel ? ( COLOR_PAIR( 1 ) | A_BOLD ) : COLOR_PAIR( 1 );
            }

            attron( date_attr );
            mvprintw( row, DATE_COL, "%s", t->date );

            // Non coloring of priority, version:
            // mvprintw(row, PRIO_COL, "%-4s", *t->priority ? t->priority :
            // "");

            // Color the priorities
            if ( *t->priority )
            {
                  char prio = t->priority[ 1 ]; // priority letter, e.g., 'A'
                  int prio_color = 0;

                  switch ( prio )
                  {
                  case 'A':
                        prio_color = 11;
                        break;
                  case 'B':
                        prio_color = 12;
                        break;
                  case 'C':
                        prio_color = 13;
                        break;
                  case 'D':
                        prio_color = 14;
                        break;
                  case 'E':
                        prio_color = 15;
                        break;
                  case 'F':
                        prio_color = 16;
                        break;
                  default:
                        prio_color = 5;
                        break; // fallback gray
                  }

                  attron( COLOR_PAIR( prio_color ) | A_BOLD );
                  mvprintw( row, PRIO_COL, "%-4s", t->priority );
                  attroff( COLOR_PAIR( prio_color ) | A_BOLD );
            }
            else
            {
                  mvprintw( row, PRIO_COL, "    " );
            }

            if ( strcmp( types[ selected_type ], "all" ) == 0 )
            {
                  //    mvprintw(row, TYPE_COL, "@%-6s", t->type); // Show
                  //    @type only for 'all'
                  if ( strcmp( types[ selected_type ], "all" ) == 0 )
                  {
                        int type_color = strcmp( t->type, "all" ) == 0
                                             ? 9
                                             : 8; // magenta for "all",
                                                  // cyan otherwise

                        mvaddch( row, TYPE_COL,
                                 '@' | COLOR_PAIR( 10 ) | A_DIM ); // Lighter @
                        attron( COLOR_PAIR( type_color ) );
                        mvprintw( row, TYPE_COL + 1, "%-6s", t->type );
                        attroff( COLOR_PAIR( type_color ) );
                  }
            }

            attroff( date_attr );

            // attron( text_attr );
            // mvprintw( row, TEXT_COL, "%s", t->text );
            // attroff( text_attr );
            //-- Prevent row overflow if text content longer than screen
            int max_text_width = COLS - TEXT_COL - 1;
            if ( max_text_width < 0 )
                  max_text_width = 0;

            mvhline( row, TEXT_COL, ' ', max_text_width ); // clear old content

            attron( text_attr );
            mvaddnstr( row, TEXT_COL, t->text,
                       max_text_width ); // print truncated
            attroff( text_attr );
            // --
            ++row;
      }

      wnoutrefresh( stdscr );
      doupdate();
}

/* ──────────────────────────────────────────────── funcs, streaming, by pipe ──
 */

bool is_pipe_input( const char *filename )
{
      struct stat st;
      if ( stat( filename, &st ) == -1 )
      {
            perror( "stat" );
            exit( 1 );
      }

      return S_ISFIFO( st.st_mode );
}

void *pipe_reader_thread( void *arg )
{
      const char *filename = (const char *)arg;

      while ( 1 )
      {
            FILE *f = fopen( filename, "r" );
            if ( !f )
            {
                  perror( "pipe open" );
                  sleep( 1 );
                  continue;
            }

            char line[ MAX_LINE ];
            while ( fgets( line, sizeof( line ), f ) )
            {
                  pthread_mutex_lock( &todo_mutex );

                  if ( todo_count >= MAX_TODOS )
                  {
                        pthread_mutex_unlock( &todo_mutex );
                        continue;
                  }

                  line[ strcspn( line, "\r\n" ) ] = '\0';

                  Todo *t = &todos[ todo_count ];
                  memset( t, 0, sizeof( Todo ) );
                  t->completed = false;
                  strncpy( t->type, "all", sizeof( t->type ) );

                  const char *at = strchr( line, '@' );
                  if ( at )
                  {
                        const char *end = at + 1;
                        while ( *end && !isspace( (unsigned char)*end ) )
                              end++;

                        size_t len = end - ( at + 1 );
                        if ( len > 0 && len < MAX_TYPE )
                        {
                              strncpy( t->type, at + 1, len );
                              t->type[ len ] = '\0';
                              add_type( t->type );
                        }

                        size_t offset = at - line;
                        memmove( line + offset, end, strlen( end ) + 1 );
                        while ( line[ offset ] == ' ' )
                              offset++;
                  }

                  time_t now    = time( NULL );
                  struct tm *tm = localtime( &now );
                  strftime( t->date, sizeof( t->date ), "%Y-%m-%d", tm );

                  strncpy( t->text, line, MAX_LINE - 1 );
                  todo_count++;

                  pthread_mutex_unlock( &todo_mutex );
                  // Keep the selected index in sync with the new todo at the
                  // bottom of the list
                  if ( auto_scroll_enabled )
                  {
                        selected_index = count_visible_items_for_type(
                                             types[ selected_type ] ) -
                                         1;
                        scroll_offset = selected_index - ( LINES - 3 );
                        if ( scroll_offset < 0 )
                              scroll_offset = 0;
                  }

                  draw_ui();
            }

            // Only happens on writer closing the pipe
            fclose( f );
      }

      return NULL;
}

/* ────────────────────────────────────────────────────────── helpers ── */

/* ───────────────────────────────────────────── main loop ── */

static void ui_loop( void )
{

      for ( int ch; ( ch = getch() ) != 'q'; )
      {

            if ( show_help )
            {
                  show_help = false;
                  draw_ui();
                  continue;
            }

            switch ( ch )
            {
            case ' ':
                  toggle_completed( selected_index );
                  break;
            case '?':
                  show_help = true;
                  break;
            case 's':
                  prompt_priority();
                  break;
            case 'p': // ascending priority
                  sort_todos_by_priority( false );
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'P': // descending priority
                  sort_todos_by_priority( true );
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'j':
                  if ( selected_index + 1 <
                       count_visible_items_for_type( types[ selected_type ] ) )
                  {
                        ++selected_index;
                  }

                  // If at bottom, not already auto-scrolling, and in stream
                  // mode → enable auto-scroll
                  if ( !auto_scroll_enabled &&
                       selected_index + 1 >= count_visible_items_for_type(
                                                 types[ selected_type ] ) &&
                       streaming_mode )
                  {
                        auto_scroll_enabled = true;
                  }
                  break;

            case 'k':
                  if ( auto_scroll_enabled )
                  {
                        auto_scroll_enabled = false;
                  }
                  if ( selected_index > 0 )
                  {
                        --selected_index;
                  }
                  break;
                  break;
            case 'h':
                  selected_type =
                      ( selected_type - 1 + type_count ) % type_count;
                  selected_index = 0;
                  break;
            case 'l':
                  selected_type  = ( selected_type + 1 ) % type_count;
                  selected_index = 0;
                  break;

            case 'd':
                  sort_todos_by_date( false ); // ascending
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'D':
                  sort_todos_by_date( true ); // descending
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;
            case 'g':
                  group_todos_by_completed();
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'G':
                  // Restore initial order from file read.
                  load_todos( todo_filename );
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 'n':
                  add_new_todo();
                  break;
            case '@':
            {
                  echo();
                  curs_set( 1 );
                  char input[ MAX_TYPE ] = { 0 };
                  move( LINES - 1, 0 );
                  clrtoeol();
                  attron( COLOR_PAIR( 2 ) | A_BOLD );
                  printw( "Jump to context @" );
                  attroff( COLOR_PAIR( 2 ) | A_BOLD );
                  getnstr( input, MAX_TYPE - 1 );
                  noecho();
                  curs_set( 0 );

                  if ( strlen( input ) > 0 )
                  {
                        // If not already known, add to types
                        bool found = false;
                        for ( int i = 0; i < type_count; ++i )
                        {
                              if ( strcmp( types[ i ], input ) == 0 )
                              {
                                    selected_type = i;
                                    found         = true;
                                    break;
                              }
                        }
                        if ( !found && type_count < MAX_TODOS )
                        {
                              types[ type_count ] = strdup( input );
                              selected_type       = type_count++;
                        }

                        selected_index = 0;
                        scroll_offset  = 0;
                  }

                  move( LINES - 1, 0 );
                  clrtoeol();
                  break;
            }
            case 'A':
                  archive_completed_todos();
                  selected_index = 0;
                  scroll_offset  = 0;
                  break;

            case 't':
                  prompt_type();
                  break;

            case 'f':
                  auto_scroll_enabled = !auto_scroll_enabled;
                  move( LINES - 1, 0 );
                  clrtoeol();
                  if ( auto_scroll_enabled )
                        mvprintw( LINES - 1, 0, "Auto-scroll: ON" );
                  else
                        mvprintw( LINES - 1, 0, "Auto-scroll: OFF" );
                  refresh();
                  napms( 800 );
                  move( LINES - 1, 0 );
                  clrtoeol();
                  break;
            }

            draw_ui();
      }
}

/* ───────────────────────────────────────────── entry ── */

int main( int argc, char **argv )
{
      if ( argc < 2 )
      {
            fprintf( stderr, "Usage: %s <todo-file>\n", argv[ 0 ] );
            return 1;
      }

      todo_filename = argv[ 1 ];

      // Optional --exec
      if ( argc == 4 && strcmp( argv[ 2 ], "--exec" ) == 0 )
      {
            exec_script = argv[ 3 ];
      }
      selected_type = 0;

      //-- streaming functionality, if activated by file being pipe
      bool is_pipe = is_pipe_input( todo_filename );
      if ( is_pipe )
      {
            streaming_mode        = true;
            types[ type_count++ ] = strdup( "all" );
            pthread_t reader;
            pthread_create( &reader, NULL, pipe_reader_thread,
                            (void *)todo_filename );
            pthread_detach( reader );
      }
      else
      {
            load_todos( todo_filename ); // normal case: load once before UI
      }

      //-- end of streaming functionality

      setlocale( LC_ALL, "" );
      initscr();
      curs_set( 0 );
      noecho();
      cbreak();
      keypad( stdscr, TRUE );

      start_color();
      use_default_colors();
      init_pair( 1, 15, -1 );   /* bright white */
      init_pair( 2, 14, -1 );   /* cyan header  */
      init_pair( 3, 220, -1 );  /* yellow date  */
      init_pair( 4, 0, 220 );   /* black on ylw */
      init_pair( 5, 245, -1 );  /* light gray   */
      init_pair( 6, 244, -1 );  /* darker gray  */
      init_pair( 7, 244, 236 ); /* gray on dark */
      init_pair( 8, 14, -1 );   /* cyan         */
      init_pair( 9, 13, -1 );   /* magenta text for 'all' category */
      init_pair( 10, 250, -1 ); /* light gray for '@' prefix */

      init_pair( 11, COLOR_RED, -1 );     // (A)
      init_pair( 12, COLOR_YELLOW, -1 );  // (B)
      init_pair( 13, COLOR_GREEN, -1 );   // (C)
      init_pair( 14, COLOR_CYAN, -1 );    // (D)
      init_pair( 15, COLOR_BLUE, -1 );    // (E)
      init_pair( 16, COLOR_MAGENTA, -1 ); // (F)

      draw_ui();
      ui_loop();

      endwin();
      return 0;
}
