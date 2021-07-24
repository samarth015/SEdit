/* --- includes --- */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdarg.h>
#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <stdbool.h>

#define CTRL_KEY(k) ((k) & 0x1f)

#define SEDIT_TAB_STOP 4

#define CLEAR_LINE "\x1b[K"
#define CLEAR_SCREEN_ESQ "\x1b[2J"
#define MOVE_CURSOR_FORMAT_ESQ "\x1b[%ld;%ldH"
#define MOVE_CURSOR_TO(X,Y) "\x1b[X;YH"
#define MOVE_CURSOR_TO_TOP_LEFT_ESQ "\x1b[H"
#define MOVE_CURSOR_TO_BOTTOM_RIGHT_ESQ "\x1b[999C\x1b[999B"
#define HIDE_CURSOR_ESQ "\x1b[?25l"
#define SHOW_CURSOR_ESQ "\x1b[?25h"
#define INVERT_COLOR_ESQ "\x1b[7m"
#define NORMAL_COLOR_ESQ "\x1b[m"
#define RED_CHARACTER_ESQ "\x1b[31m"
#define DEFAULT_CHARACTER_ESQ "\x1b[39m"
#define CHANGE_COLOR_FORMAT_ESQ "\x1b[%dm"

#define CLEAR_SCREEN() write(STDOUT_FILENO, CLEAR_SCREEN_ESQ, 4)
#define REPOSITION_CURSOR() write(STDOUT_FILENO, MOVE_CURSOR_TO_TOP_LEFT_ESQ, 3)
#define MOVE_CURSOR_TO_BOTTOM_RIGHT() (write(STDOUT_FILENO, MOVE_CURSOR_TO_BOTTOM_RIGHT_ESQ, 12) == 12)


/* --- defines --- */

enum editor_hightlight{
	HL_NORMAL = 0,
	HL_COMMENT,
	HL_STRING,
	HL_NUMBER,  
	HL_MATCH,
	HL_KEYWORD_1,
	HL_KEYWORD_2,
};

enum editor_key {
	BACKSPACE = 127,
	ESC = 0x1b,
	ARROW_UP = 1000,
	ARROW_DOWN,
	ARROW_LEFT,
	ARROW_RIGHT,
	PAGE_UP,
	PAGE_DOWN,
	HOME,
	END,
	DEL_KEY
};

struct erow{
	long idx;
	long size;
	char *characters;
	long rsize;
	char *render;
	unsigned char *hl;
	int hl_open_comment;
};

typedef struct erow erow;

struct config{
	struct termios orig_termios;
	long screen_rows, screen_cols;
	long cx, cy, ry;
	long num_rows;
	long row_offset, col_offset;
	erow *rows;
	char *file_name;
	struct editor_syntax *syntax;
	char status_msg[80];
	time_t status_msg_time;
	size_t modified;
	bool quit_pressed_last;
};

struct editor_syntax {
	char *file_type;
	char **file_match;
	char **keywords;
	char *singleline_comment_start; 
	char *multiline_comment_start;
	char *multiline_comment_end;
	int flags;
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)


void die(const char *s) {
	CLEAR_SCREEN();
	REPOSITION_CURSOR();

	perror(s);
	exit(1);
}

/* --- data --- */

struct config St;    // St for state/settings

const char *name_ascii_art[]=  

 {  "   ███████    ███████ ██████  ██ ████████ ",
	"   ██         ██      ██   ██ ██    ██    ",
	"   ███████    █████   ██   ██ ██    ██    ",
	"        ██    ██      ██   ██ ██    ██    ",
	"   ███████    ███████ ██████  ██    ██    ",
	"     Sam's    Editor                      "};


char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", NULL };

char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else", "#include", "#define",
  "struct", "union", "typedef", "static", "enum", "class", "case", "const",

  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL
};

struct editor_syntax HLDB[] = { {
		"C/C++",
		C_HL_extensions,
		C_HL_keywords,
		"//", "/*", "*/",
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS,
	}
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* --- prototypes --- */

void editor_set_status_message(const char *format, ...);
void editor_refresh_screen();
char* editor_prompt(const char *format, void (*callback)(char *,int));
void editor_update_syntax(erow *);
int editor_syntax_to_color(int); 
void editor_evaluate_ry();
void editor_select_syntax_highlight();

/* --- ROW OPERATIONS --- */

bool cursor_below_last_line(){
	return St.cx == St.num_rows;
}

void editor_update_row(erow *row){
	long tabs = 0, ctrls = 0, misellanous = 0;
	char *seq = row->characters;

	for(long y = 0; y < row->size; y++){
		tabs += seq[y] == '\t';
		ctrls += (0 <= seq[y] && seq[y] <= 26);
		misellanous += (28 <= seq[y] && seq[y] <= 31);
	}

	free(row->render);
	row->render = malloc(row->size + tabs*(SEDIT_TAB_STOP - 1) + ctrls + misellanous + 1);

	long y;
	long idx = 0;
	for(y = 0; y < row->size; y++){
		if(seq[y] == '\t'){
			do{
				row->render[idx++] = ' ';
			} while(idx % SEDIT_TAB_STOP);
		}
		else if(0 <= seq[y] && seq[y] <= 26){
			row->render[idx++] = '^';
			row->render[idx++] = '@' + seq[y];
		}
		else if(28 <= seq[y] && seq[y] <= 31){
			row->render[idx++] = '^';
			row->render[idx++] = '?';
		}
		else{
			row->render[idx++] = row->characters[y];
		}
	}

	row->render[idx] = '\0';
	row->rsize = idx;

	row->hl = malloc(row->rsize);
	editor_update_syntax(row);
}

void editor_insert_row(long at,char *s){

	St.rows = realloc(St.rows ,sizeof(erow) * (St.num_rows + 1));

	memmove(St.rows + at + 1, St.rows + at, sizeof(erow)*(St.num_rows - at));
	for (long y = at + 1; y <= St.num_rows; y++) St.rows[y].idx++;


	St.rows[at].idx = at;

	St.rows[at].characters = s;
	St.rows[at].size = strlen(s);

	St.rows[at].rsize = 0;
	St.rows[at].render = NULL;
	St.rows[at].hl = NULL;
	St.rows[at].hl_open_comment = 0;

	editor_update_row(St.rows + at);

	St.num_rows++;
	St.modified++;
}

void editor_row_insert_character(erow *row, long at, int ch){
	row->characters = realloc(row->characters, row->size + 2);

	long i = row->size;
	while( i > at ){
		row->characters[i] = row->characters[i-1];
		i--;
	}
	row->characters[i] = ch;

	row->size++;
	row->characters[row->size] = '\0';

	editor_update_row(row);
	St.modified++;
}

void editor_row_append_string(erow *row, const char *str, size_t len){
	row->characters = realloc(row->characters, row->size + len + 1);
	memcpy(row->characters + row->size, str, len);
	row->size += len;
	row->characters[row->size] = '\0';
	editor_update_row(row);
	St.modified++;
}

void editor_row_delete_character(erow *row, long at){

	long i = at + 1;
	while(i < row->size + 1){
		row->characters[i-1] = row->characters[i];
		i++;
	}
	row->size--;

	editor_update_row(row);
	St.modified++;
}

void editor_free_row(erow *row){
	free(row->characters);
	free(row->render);
	free(row->hl);
}

void editor_delete_row(long at){
	editor_free_row(St.rows + at);
	memmove(St.rows + at, St.rows + at + 1, sizeof(erow)*(St.num_rows - at - 1));
	for (int y = at; y < St.num_rows - 1; y++) St.rows[y].idx--;

	St.num_rows--;
	St.modified++;
}

/* --- editor operations --- */

void editor_insert_newline_at_cursor(){
	char *new_line;
	if(cursor_below_last_line()){
		new_line = malloc(1);
		*new_line = '\0';
		editor_insert_row(St.num_rows, new_line);
	}
	else{
		erow *row = St.rows + St.cx;
		char *new_line = strdup(row->characters + St.cy);
		row->size = St.cy;
		row->characters[row->size] = '\0';
		editor_insert_row(St.cx + 1, new_line);
		row = St.rows + St.cx; //editor_insert realloc()s rows so reassigning
		editor_update_row(row); 
	}
	St.cy = 0;
	St.cx++;
}

void editor_insert_char_at_cursor(int ch){
	if(cursor_below_last_line()){
		char *new_row = malloc(2);
		sprintf(new_row, "%c", ch);
		editor_insert_row(St.num_rows, new_row);
	}
	else{
		editor_row_insert_character(St.rows + St.cx, St.cy, ch); 
	}
	St.cy++;
}

void editor_delete_character_at_cursor(){
	if(cursor_below_last_line()) return;

	erow *row = St.rows + St.cx;
	if(St.cx == St.num_rows - 1 && St.cy == row->size) return; //Cursor at bottom right

	if(St.cy == row->size){
		editor_row_append_string(row, (row+1)->characters, (row+1)->size);
		editor_delete_row(St.cx+1);
	}
	else{
		editor_row_delete_character(St.rows + St.cx, St.cy);
	}

}

/*  --- file io --- */ 

void editor_open(const char *filename){
	free(St.file_name);
	St.file_name = strdup(filename);

	editor_select_syntax_highlight();

	FILE *file = fopen(filename, "r");
	if(file == NULL) die("editor_open");

	char *line = NULL;
	size_t linecap = 0;
	long linelen;

	while((linelen = getline(&line, &linecap, file)) != -1){
		while(linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
			linelen--;
		line[linelen] = '\0';
		editor_insert_row(St.num_rows, line);
		line = NULL; linecap = 0;
	}
	fclose(file);
	St.modified = 0;
}

void editor_rows_to_string(char **full_string, long *total_length){
	long total_len = 0;
	for(long x = 0; x < St.num_rows; x++) total_len += strlen(St.rows[x].characters) + 1;

	char *full_str = malloc(total_len + 1);

	char *buf_ptr = full_str;
	for(long x = 0; x < St.num_rows; x++){
		long curr_row_len = St.rows[x].size;
		memcpy(buf_ptr, St.rows[x].characters, curr_row_len);
		buf_ptr += curr_row_len;
		*buf_ptr = '\n';
		buf_ptr++;
	}
	full_str[total_len] = '\0';

	*full_string = full_str;
	*total_length = total_len;
}


void editor_save_file(){
	if(St.file_name == NULL) {
		St.file_name = editor_prompt("Save as : %s  (Cancel = Esc)", NULL);
		editor_select_syntax_highlight();
	}
	if(St.file_name == NULL) {
		editor_set_status_message("Save aborted");
		return;
	}

	char *buf; long len;
	editor_rows_to_string(&buf, &len);
	int fd = open(St.file_name, O_RDWR | O_CREAT, 0644);
	if(fd != -1){
		if(ftruncate(fd, len) != -1){
			if(write(fd, buf, len) != -1){
				close(fd);
				free(buf);
				editor_set_status_message("FILE SAVED. %d bytes written.", len);
				St.modified = 0;
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editor_set_status_message("SAVE FAILED. I/O error: %s", strerror(errno));
}

/* editor find */

void editor_find_callback(char* query, int key){
	static int saved_hl_line;
	static char *saved_hl = NULL;
	if (saved_hl) {
		memcpy(St.rows[saved_hl_line].hl, saved_hl, St.rows[saved_hl_line].rsize);
		free(saved_hl);
		saved_hl = NULL;
	}

	static int direction = 1;
	static int last_match_line = -1;

	if( key == '\r' || key == ESC ) return;
	else if( key == ARROW_RIGHT || key == ARROW_DOWN ) direction = 1;
	else if( key == ARROW_LEFT || key == ARROW_UP ) direction = -1;
	else {
		last_match_line = -1;
		direction = 1;
	}

	int current = last_match_line;
	char *match;
	erow *row;
	for(int i = 0; i < St.num_rows; i++){
		current += direction;
		if(current == -1) current = St.num_rows - 1;
		else if(current == St.num_rows) current = 0;

		row = St.rows + current;
		match = strstr(row->characters, query);
		if(match) break;
	}

	if(match){
		St.cx = last_match_line = current;
		St.cy = match - row->characters; 
		St.row_offset = (current - St.screen_rows/2);
		if(St.row_offset < 0) St.row_offset = 0;

		editor_evaluate_ry();
		saved_hl_line = current;
		saved_hl = malloc(row->rsize);
		memcpy(saved_hl, row->hl, row->rsize);
		memset(row->hl + St.ry, HL_MATCH, strlen(query));
	}
}

void editor_find(){

	long cx_orig = St.cx;
	long cy_orig = St.cy;
	long row_offset_orig = St.row_offset;
	long col_offset_orig = St.col_offset;

	char *query = editor_prompt("SEARCH : %s (Use Esc/Enter/ArrowKeys)", editor_find_callback);

	if(query){
		free(query);
	}
	else{
		St.cx = cx_orig;
		St.cy = cy_orig;
		St.row_offset = row_offset_orig;
		St.col_offset = col_offset_orig;
	}
}

/* --- appendable string --- */ 

struct appendable_str{
	char *buf;
	long len;
};
#define INIT_APPENDABLE_STR { NULL, 0 };

void append(struct appendable_str *to, const char *str, long len){
	if(len <= 0) return;
	to->buf = realloc(to->buf, to->len + len);

	if(to->buf == NULL) return;
	memcpy(to->buf + to->len, str, len);
	to->len += len;
}

void free_appendable_str(struct appendable_str *str){
	free(str->buf);
}

int get_cursor_position(long *X, long *Y){
	if( write(STDOUT_FILENO, "\x1b[6n", 4) != 4 ) return -1;

	char buf[32];
	char ch;

	unsigned i = 0;
	while (read(STDIN_FILENO, &ch, 1) == 1 && ch != 'R') {
		buf[i] = ch;
		i++;
	}

	buf[i] = '\0';

	if(buf[0] != '\x1b' || buf[1] != '[') return -1;
	if(sscanf(buf + 2, "%ld;%ld", X, Y) != 2) return -1;

	return 0;
}

int get_window_size(long *X, long *Y){
	struct winsize ws;
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
		if( ! MOVE_CURSOR_TO_BOTTOM_RIGHT() ) return -1;
		return get_cursor_position(X, Y);
	}
	else{
		*X = ws.ws_row;
		*Y = ws.ws_col;
		return 0;
	}
}


/* --- init --- */

void init_editor(){
	St.num_rows = 0;
	St.cx = St.cy = St.ry = 0;
	St.row_offset = St.col_offset = 0;
	St.rows = NULL;
	St.file_name = NULL;
	St.status_msg[0] = '\0';
	St.status_msg_time = 0;
	St.modified = 0;
	St.quit_pressed_last = false;
	St.syntax = NULL;

	if(get_window_size(&St.screen_rows, &St.screen_cols) == -1)
		die("get_window_size");

	St.screen_rows -= 2;    //Leaving last 2 lines for status bar and message.
}


/* --- output --- */

void editor_evaluate_ry(){
	if(cursor_below_last_line()){
		St.ry = 0;
	}
	else{
		erow *row = St.rows + St.cx;
		long ry = 0;
		for(long y = 0; y < St.cy; y++){
			if(row->characters[y] == '\t'){
				do{
					ry++;
				}
				while(ry % SEDIT_TAB_STOP);
			}
			else if(row->characters[y] <= 31){
				ry += 2;
			}
			else{
				ry++;
			}
		}
		St.ry = ry;
	}
}

void editor_scroll(){
	editor_evaluate_ry();

	if(St.cx < St.row_offset) 
		St.row_offset = St.cx;
	else if(St.cx >= St.row_offset + St.screen_rows)
		St.row_offset = St.cx - St.screen_rows + 1;
	if(St.ry < St.col_offset)
		St.col_offset = St.ry;
	else if(St.ry >= St.col_offset + St.screen_cols)
		St.col_offset = St.ry - St.screen_cols + 1;
}

long editor_draw_welcome_message_ascii_art(struct appendable_str *astr){
	long x;
	for(x = 0; x < 6; x++){
		append(astr, CLEAR_LINE, strlen(CLEAR_LINE));

		append(astr, "---", 3);
		append(astr, name_ascii_art[x], strlen(name_ascii_art[x]));

		append(astr, "\n\r", 2);
	}
	return x;
}

bool editor_file_content_fills_whole_screen(){
	return (St.num_rows - St.row_offset) >= St.screen_rows;
}

long editor_draw_file_contents(struct appendable_str *astr){

	long X;  // X is index of last row/line of file to be drawn.
	if(editor_file_content_fills_whole_screen())
		X = St.row_offset + St.screen_rows - 1;
	else
		X = St.num_rows - 1;

	for(long x = St.row_offset; x <= X; x++){
		append(astr, CLEAR_LINE, strlen(CLEAR_LINE));

		erow *row = St.rows + x;
		unsigned char *hl = row->hl;
		char *rseq = row->render;
		long len = row->rsize;
		int current_color = -1;

		if(len > (long)St.screen_cols) len = St.screen_cols;

		for(long y = St.col_offset; y < len; y++){
			if(hl[y] == HL_NORMAL){
				if(current_color != -1){
					current_color = -1;
					append(astr, DEFAULT_CHARACTER_ESQ, strlen(DEFAULT_CHARACTER_ESQ));
				}
				append(astr, rseq + y, 1);
			}
			else{
				int color = editor_syntax_to_color(hl[y]);
				if(current_color != color){
					current_color = color;
					char buf[16];
					int clen = snprintf(buf, sizeof(buf), CHANGE_COLOR_FORMAT_ESQ, color);
					append(astr , buf, clen);
				}
				append(astr, rseq + y, 1);
			}
		}
		append(astr, DEFAULT_CHARACTER_ESQ, strlen(DEFAULT_CHARACTER_ESQ));
		append(astr, "\n\r", 2);
	}
	return X + 1;
}

void editor_draw_empty_rows(struct appendable_str *astr, long first_empty_row){
	for(long x = first_empty_row - St.row_offset; x < St.screen_rows ; x++){
		append(astr, CLEAR_LINE, strlen(CLEAR_LINE));
		append(astr, "---\n\r", 5);
	}
}

void editor_draw_status_bar(struct appendable_str *astr){
	append(astr, INVERT_COLOR_ESQ, strlen(INVERT_COLOR_ESQ));

	char status[80];
	char rstatus[80];

	int len = snprintf(status, sizeof(status), "%.20s%s -- %ld lines",
			St.file_name ? St.file_name : "[NO NAME]",
			( St.modified ? "(+)" : ""), 
			St.num_rows);

	int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %ld/%ld",
			( St.syntax ? St.syntax->file_type : "No filetype" ),
			St.cx + 1, 
			St.num_rows);

	if(len > St.screen_cols) len = St.screen_cols;
	append(astr, status, len);

	while(len < St.screen_cols - rlen){
		append(astr, " ", 1);
		len++;
	}

	if(rlen == St.screen_cols - len) 
		append(astr, rstatus, rlen);

	append(astr, NORMAL_COLOR_ESQ, strlen(NORMAL_COLOR_ESQ));
	append(astr, "\r\n", 2);
}

void editor_draw_status_message(struct appendable_str *astr){
	append(astr, CLEAR_LINE, strlen(CLEAR_LINE));

	int len = strlen(St.status_msg);
	if(len > St.screen_cols) len = St.screen_cols;
	if( time(NULL) - St.status_msg_time < 5 )
		append(astr,St.status_msg, len);
}

void editor_draw_rows(struct appendable_str *astr){
	long first_empty_row;
	if(St.num_rows == 0){
		first_empty_row = editor_draw_welcome_message_ascii_art(astr);
	}
	else{
		first_empty_row = editor_draw_file_contents(astr);
	}
	editor_draw_empty_rows(astr, first_empty_row);
	editor_draw_status_bar(astr);
	editor_draw_status_message(astr);
}

void editor_refresh_screen(){
	editor_scroll();

	struct appendable_str astr = INIT_APPENDABLE_STR;

	append(&astr, HIDE_CURSOR_ESQ, strlen(HIDE_CURSOR_ESQ));
	append(&astr, MOVE_CURSOR_TO_TOP_LEFT_ESQ, strlen(MOVE_CURSOR_TO_TOP_LEFT_ESQ));

	editor_draw_rows(&astr);

	char cursor_position_update[32];

	snprintf( cursor_position_update, 
			sizeof(cursor_position_update), 
			MOVE_CURSOR_FORMAT_ESQ, 
			St.cx - St.row_offset + 1, 
			St.ry - St.col_offset + 1 );

	append(&astr,cursor_position_update, strlen(cursor_position_update));

	append(&astr, SHOW_CURSOR_ESQ, strlen(SHOW_CURSOR_ESQ));

	write(STDOUT_FILENO, astr.buf, astr.len);

	free_appendable_str(&astr);
}

/* --- terminal --- */

void disable_raw_mode(){
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &St.orig_termios) == -1 )
		die("tcsetattr");
}


void enable_raw_mode(){
	if(tcgetattr( STDIN_FILENO, &St.orig_termios ) == -1)
		die("tcgetattr");

	struct termios raw = St.orig_termios;

	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);	
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag &= ~(CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");

	atexit(disable_raw_mode);
}

/* --- syntax highlight --- */

bool is_separator(int c) {
	return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[]{};", c) != NULL;
}

void editor_update_syntax(erow *row){

	row->hl = realloc(row->hl, row->rsize);
	memset(row->hl, HL_NORMAL, row->rsize);

	if(St.syntax == NULL) return;	

	unsigned char *hl = row->hl;
	char **keywords = St.syntax->keywords;

	char *slcs = St.syntax->singleline_comment_start;
	char *mlcs = St.syntax->multiline_comment_start;
	char *mlce = St.syntax->multiline_comment_end;

	int slcs_len = strlen(slcs);
	int mlcs_len = strlen(mlcs);
	int mlce_len = strlen(mlce);


	bool is_prev_sep = true;
	char inside_string = 0; 
	char inside_comment = (row->idx > 0 && St.rows[row->idx - 1].hl_open_comment);

	int y = 0;
	while(y < row->rsize){
		char ch = row->render[y];
		unsigned char prev_hl = ( y > 0 ? row->hl[y-1] : HL_NORMAL );

		if(slcs && !inside_string && !inside_comment){
			if(strncmp(slcs, row->render + y, slcs_len) == 0){
				memset(row->hl + y, HL_COMMENT, row->rsize - y);
				break;
			}
		}

		if(St.syntax->flags & HL_HIGHLIGHT_STRINGS){
			if(inside_string){
				hl[y] = HL_STRING;
				if(inside_string == ch && row->render[y-1] != '\\') inside_string = 0;
				is_prev_sep = true;
				y++;
				continue;
			}
			else if(ch == '"' || ch == '\''){
				inside_string = ch;
				row->hl[y] = HL_STRING;
				y++;
				continue;
			}
		}

		if(mlcs && mlcs  && !inside_string){
			if(inside_comment){
				hl[y] = HL_COMMENT;
				if(! strncmp(mlce, row->render + y, mlce_len)){
					memset(hl + y, HL_COMMENT, mlce_len);
			 		y += mlce_len;
					inside_comment = 0;
				}
				else{
					y++;
					continue;
				}
			}
			else if(! strncmp(row->render + y, mlcs, mlcs_len)){
				memset(hl + y, HL_COMMENT, mlcs_len);
				y += mlcs_len;
				inside_comment = 1;
				continue;
			}
		}

		if(St.syntax->flags & HL_HIGHLIGHT_NUMBERS){
			if((isdigit(ch) && ( is_prev_sep || prev_hl == HL_NUMBER )) || 
					( ch == '.' && prev_hl == HL_NUMBER )){
				row->hl[y] = HL_NUMBER;
				y++;
				is_prev_sep = false;
				continue;
			}
		}

		if(is_prev_sep){
			char **kws = keywords;
			int kw_len; 
			bool is_keyword_2;
			while(*kws){
				kw_len = strlen(*kws);
				is_keyword_2 = (*kws)[kw_len - 1] == '|';
				if(is_keyword_2) kw_len--;
				if(! strncmp(*kws, row->render + y, kw_len) && is_separator(row->render[y + kw_len])) break;
				kws++;
			}

			if(*kws){
				int HL_KEYWORD = ( is_keyword_2 ? HL_KEYWORD_2 : HL_KEYWORD_1 );
				memset(row->hl + y, HL_KEYWORD, kw_len);
				y += kw_len;
				is_prev_sep = false;
				continue;
			}
		}

		is_prev_sep = is_separator(ch);
		y++;
	} 
	int changed = (row->hl_open_comment != inside_comment);
	row->hl_open_comment = inside_comment;
	if (changed && row->idx + 1 < St.num_rows)
		editor_update_syntax(&St.rows[row->idx + 1]);
}

int editor_syntax_to_color(int hl) {
	switch (hl) {
		case 	   HL_COMMENT    : return 35;
		case 	   HL_STRING     : return 36;
		case       HL_NUMBER     : return 33;
		case       HL_MATCH      : return 34;
		case 	   HL_KEYWORD_1  : return 31;
		case 	   HL_KEYWORD_2  : return 32;
		default                  : return 37;
	}
}

void editor_select_syntax_highlight(){
	St.syntax = NULL;
	if(St.file_name == NULL) return;

	char *ext = strchr(St.file_name, '.');
	if(ext == NULL) return;

	size_t i;
	for(i = 0; i < HLDB_ENTRIES; i++){
		char **entry_match = HLDB[i].file_match;
		while(*entry_match){
			if(strcmp(*entry_match, ext) == 0){
				St.syntax = &HLDB[i];

				for(long x = 0; x < St.num_rows; x++) 
					editor_update_syntax(St.rows + x);

				return;
			}
			entry_match++;
		}
	}
}

/* --- input --- */


int editor_read_key(){
	int nread;
	char ch;
	while((nread = read(STDIN_FILENO, &ch, 1)) != 1){
		if(nread == -1 && errno != EAGAIN) 
			die("read");
	}

	if(ch == ESC){
		char buf[3];
		if(read(STDIN_FILENO, &buf[0], 1) == 0) return ESC;
		if(read(STDIN_FILENO, &buf[1], 1) == 0) return ESC;

		if(buf[0] == '['){

			if('0' <= buf[1] && buf[1] <= '9'){
				if(read(STDIN_FILENO, &buf[2], 1) == 0) return ESC;

				if(buf[2] == '~'){
					switch(buf[1]){
						case '1': return HOME;
						case '3': return DEL_KEY;
						case '4': return END;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME;
						case '8': return END;
					}
				}
			}
			else{
				switch(buf[1]){
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'D': return ARROW_LEFT;
					case 'C': return ARROW_RIGHT;
					case 'H': return HOME;
					case 'F': return END;
				}
			}
			return ESC;
		}
		else if(buf[0] == 'O') {
			switch (buf[1]) {
				case 'H': return HOME;
				case 'F': return END;
			}
		}
		else{
			return ESC;
		}
	}
	return ch;
}

void editor_move_cursor(int key){
	erow *this_row = ( St.cx >= St.num_rows ? NULL : St.rows + St.cx);
	switch(key){
		case ARROW_LEFT:
			if(St.cy > 0) St.cy--;
			else if(St.cx > 0){
				St.cx--;
				St.cy = St.rows[St.cx].size;
			}
			break;

		case ARROW_UP:
			if(St.cx > 0) St.cx--;
			break;

		case ARROW_DOWN:
			if(St.cx < St.num_rows) St.cx++;
			break;

		case ARROW_RIGHT: 
			if(this_row && St.cy < this_row->size) St.cy++;
			else if(St.cx < St.num_rows){
				St.cx++;
				St.cy = 0;
			}
			break;

	}
	this_row = ( St.cx >= St.num_rows ? NULL : St.rows + St.cx);  
	long len = (this_row ? this_row->size : 0 );
	if( St.cy > len ) St.cy = len;

}

void editor_process_keypress(){
	int ch = editor_read_key();

	switch(ch){
		case '\r':
			editor_insert_newline_at_cursor();
			break;

		case CTRL_KEY('q'):
			if(St.modified && !St.quit_pressed_last){
				editor_set_status_message("WARNING -- File unsaved, changes will be lost. Press Ctrl-Q again to force quit");
				St.quit_pressed_last = true;
				return;
			}
			else{
				CLEAR_SCREEN();
				REPOSITION_CURSOR();
				exit(0);
			}
			break;

		case CTRL_KEY('f'):
			editor_find();
			break;

		case CTRL_KEY('s'):
			editor_save_file();
			break;

		case PAGE_UP:
			St.row_offset -= St.screen_rows - 1;
			St.cx -= St.screen_rows - 1;
			if(St.row_offset < 0) St.row_offset = 0;
			if(St.cx < 0) St.cx = 0;
			if(St.cy > St.rows[St.cx].size) St.cy = St.rows[St.cx].size;
			break;

		case PAGE_DOWN:
			St.row_offset += St.screen_rows - 1;
			St.cx += St.screen_rows - 1;
			if(St.row_offset > St.num_rows - St.screen_rows - 1) 
				St.row_offset = St.num_rows - St.screen_rows;
			if(St.cx > St.num_rows - St.screen_rows - 1) 
				St.cx = St.row_offset + St.screen_rows - 1;
			if(St.cy > St.rows[St.cx].size) St.cy = St.rows[St.cx].size;
			break;

		case HOME:
			St.cy = 0;
			break;

		case END:
			if(St.cx < St.num_rows) St.cy = St.rows[St.cx].size;
			break;

		case DEL_KEY: 
			editor_delete_character_at_cursor();
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
			editor_move_cursor(ARROW_LEFT);
			editor_delete_character_at_cursor();
			break;


		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editor_move_cursor(ch);
			break;

		case ESC:
		case CTRL_KEY('l'):
			break;

		default:
			editor_insert_char_at_cursor(ch);
			break;
	}
	St.quit_pressed_last = false;
}

void editor_set_status_message(const char *format, ...){
	va_list ap;
	va_start(ap, format);
	vsnprintf(St.status_msg, sizeof(St.status_msg), format, ap);
	va_end(ap);
	St.status_msg_time = time(NULL);
}

char* editor_prompt(const char *prompt, void (*callback)(char *,int)){
	size_t bufsize = 128, buflen = 0;
	char *input_buffer = malloc(bufsize);
	input_buffer[0] = '\0';

	while(1){
		editor_set_status_message(prompt, input_buffer);
		editor_refresh_screen();

		int key = editor_read_key();

		if(key == '\r' && buflen > 0){
			editor_set_status_message("");
			if(callback) callback(input_buffer, key);
			return input_buffer;
		}
		else if(key == ESC){
			free(input_buffer);
			editor_set_status_message("");
			if(callback) callback(input_buffer, key);
			return NULL;
		}
		else if(key == BACKSPACE || key == CTRL_KEY('h') || key == DEL_KEY){
			editor_set_status_message("");
			if(buflen > 0) input_buffer[--buflen] = '\0';
		}
		else if(!iscntrl(key) && key < 128){
			if(buflen == bufsize - 1){
				bufsize *= 2;
				input_buffer = realloc(input_buffer, bufsize);
			}
			input_buffer[buflen++] = key;
			input_buffer[buflen] = '\0';
		}
		if(callback) callback(input_buffer, key);
	}
}


/* --- main --- */

int main(int argc, char *argv[])
{
	enable_raw_mode();
	init_editor();

	if(argc >= 2) editor_open(argv[1]);

	editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = search");

	while(1){
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
