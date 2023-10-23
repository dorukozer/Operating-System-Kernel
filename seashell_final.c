#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h> 
#include <limits.h>
const char * sysname = "seashell";

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};
struct command_t {
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3]; // in/out redirection
	struct command_t *next; // for piping
};
/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t * command)
{
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");
	for (i = 0; i < 3; i++)
		printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i = 0; i < command->arg_count; ++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}


}
/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i = 0; i < 3; ++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next = NULL;
	}
	free(command->name);
	free(command);
	return 0;
}
/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}
/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);
	while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
		buf[--len] = 0; // trim right whitespace

	if (len > 0 && buf[len - 1] == '?') // auto-complete
		command->auto_complete = true;
	if (len > 0 && buf[len - 1] == '&') // background
		command->background = true;

	char *pch = strtok(buf, splitters);
	command->name = (char *)malloc(strlen(pch) + 1);
	if (pch == NULL)
		command->name[0] = 0;
	else
		strcpy(command->name, pch);

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;
	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch) break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		if (len == 0) continue; // empty arg, go for next
		while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL) arg[--len] = 0; // trim right whitespace
		if (len == 0) continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|") == 0)
		{
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t') index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0)
			continue; // handled before

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<')
			redirect_index = 0;
		if (arg[0] == '>')
		{
			if (len > 1 && arg[1] == '>')
			{
				redirect_index = 2;
				arg++;
				len--;
			}
			else redirect_index = 1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"')
			|| (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}
		command->args = (char **)realloc(command->args, sizeof(char *)*(arg_index + 1));
		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;
	return 0;
}
void prompt_backspace()
{
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}
/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


	//FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state = 0;
	buf[0] = 0;
	while (1)
	{
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c == 9) // handle tab
		{
			buf[index++] = '?'; // autocomplete
			break;
		}

		if (c == 127) // handle backspace
		{
			if (index > 0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c == 27 && multicode_state == 0) // handle multi-code keys
		{
			multicode_state = 1;
			continue;
		}
		if (c == 91 && multicode_state == 1)
		{
			multicode_state = 2;
			continue;
		}
		if (c == 65 && multicode_state == 2) // up arrow
		{
			int i;
			while (index > 0)
			{
				prompt_backspace();
				index--;
			}
			for (i = 0; oldbuf[i]; ++i)
			{
				putchar(oldbuf[i]);
				buf[i] = oldbuf[i];
			}
			index = i;
			continue;
		}
		else
			multicode_state = 0;

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1) break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}
	if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
		index--;
	buf[index++] = 0; // null terminate string

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	// print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}
int process_command(struct command_t *command);
int main()
{
	while (1)
	{
		struct command_t *command = malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code == EXIT) break;

		code = process_command(command);
		if (code == EXIT) break;

		free_command(command);
	}

	printf("\n");
	return 0;
}

int process_command(struct command_t *command)
{
	int r;
	if (strcmp(command->name, "") == 0) return SUCCESS;

	if (strcmp(command->name, "exit") == 0)
		return EXIT;

	if (strcmp(command->name, "cd") == 0)
	{
		if (command->arg_count > 0)
		{
			r = chdir(command->args[0]);
			if (r == -1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			return SUCCESS;
		}
	}

	if (strcmp(command->name, "highlight") == 0) {
		//seashell> highlight <word> <r | g | b> <filename>

		FILE *textfile = fopen(command->args[2], "r");
		if (textfile != NULL) {
			char *colorval, *normal, line[512];
			char input_color;

			if (strlen(command->args[1]) == 1) { //check if single letter
				input_color = *command->args[1]; //char* to char conversion
			}
			else {
				printf("Invalid color.\n"); return EXIT;
			}

			switch (input_color) { //char conversion allows me to use switch
			case 'r': colorval = "\033[0;31m"; break;
			case 'g': colorval = "\033[0;32m"; break;
			case 'b': colorval = "\033[0;34m"; break;
			default: printf("Invalid color.\n"); return EXIT;
			}

			normal = "\033[0m";

			while (fgets(line, sizeof(line), textfile) != NULL) {

				char buffer[sizeof(line)]; //used to print the modified string

				char* tokens = strtok(line, " ");//the text must have spacing between the punctuation marks for this to work correctly; otherwise it gets too complicated

				char search_word[32]; //string ops work best with char arrays, so we're using those.

				strcpy(search_word, command->args[0]);

				bool valid = false; //tracks if we should be printing a line or not.

				int i;

				int search_length = strlen(search_word);

				for (i = 0; tokens != NULL; i++) { //Searching for each token
					
					int k;
					bool word_exists = true; //used for altering the token. If set to false during the if statement below, the word does not match
					char token_word[32];
					strcpy(token_word, tokens);

					if (strlen(tokens) != search_length) {//streamlines comparison process by checking lengths first, also prevents arrayIndexOutOfBounds
						word_exists = false;
					} else {
						for (k = 0; k < search_length; k++) {
							if (token_word[k] != search_word[k]) {//char by char comparison, case insensitive
								if (token_word[k] + 32 != search_word[k]) {
									word_exists = false;
								}
							}
						}
					}

					if (word_exists) {//modifies the token and adds it to the buffer array. if it's false, token is added without modification.

						char colored_word[64];

						strcpy(colored_word, colorval);

						strcat(colored_word, tokens);
						strcat(colored_word, normal);
						strcat(colored_word, " ");
						strcat(buffer, colored_word);

						valid = true;//flag that the searched word was found within a line.

					}
					else {
						char token_buffer[32];

						strcpy(token_buffer, tokens);

						strcat(token_buffer, " ");
						strcat(buffer, token_buffer);
					}

					tokens = strtok(NULL, " ");
				}

				if (valid) {//print the buffer onto the shell if a match was found.
					printf("%s \n", buffer);
				}

				memset(buffer, 0, sizeof(buffer)); //clear the buffer array for the next line.

			}

			fclose(textfile);
			return SUCCESS;
		}
		else {
			printf("File could not be opened.");
			return EXIT;
		}

	}

	pid_t pid = fork();
	if (pid == 0) // child
	{
		/// This shows how to do exec with environ (but is not available on MacOs)
		// extern char** environ; // environment variables
		// execvpe(command->name, command->args, environ); // exec+args+path+environ

		/// This shows how to do exec with auto-path resolve
		// add a NULL argument to the end of args, and the name to the beginning
		// as required by exec

		// increase args size by 2


		command->args = (char **)realloc(
			command->args, sizeof(char *)*(command->arg_count += 2));

		// shift everything forward by 1
		for (int i = command->arg_count - 2; i > 0; --i)
			command->args[i] = command->args[i - 1];

		// set args[0] as a copy of name
		command->args[0] = strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count - 1] = NULL;

		char * PATH = getenv("PATH"); // return to PATH variable
		char* token = strtok(PATH, ":"); // tokenize the PATH variable so that we can search in them for linux commands
		char *new_path = malloc(8);
		/* walk through other tokens */

		int i = 0;
		/* walk through other tokens */
		while (token != NULL) {    //searching at the path variable
			new_path = realloc(new_path, strlen(token) + strlen(command->args[0]) + 2);
			char* place = strdup(token);
			strcpy(new_path, place);
			strcat(new_path, "/");

			strcat(new_path, command->args[0]);

			if (access(new_path, X_OK) == 0) { // if linux command is found execute that command
				execv(new_path, command->args);
			}
			token = strtok(NULL, ":");
		}

		exit(0);
		/// TODO: do your own exec with path resolving using execv()
	}
	else
	{

		if (!command->background)
			wait(0); // wait for child process to finish


		if (strcmp(command->name, "kdiff") == 0) {  //implement kdiff
			char* flag = strdup(command->args[0]);// this takes the parameter -a or -b
			char* first_txt = strdup(command->args[1]); // name of first txt 
			char* second_txt = strdup(command->args[2]);// name of second txt 
			char* a = "./";
			char* first_path = strdup(a);
			char* second_path = strdup(a);
			// these 4 lines are for finding first and second txt files relative to the current direcotry 
			first_path = realloc(first_path, strlen(first_txt) + 3);
			second_path = realloc(second_path, strlen(second_txt) + 3);
			strcat(first_path, first_txt);
			strcat(second_path, second_txt);
			FILE * fp1; // first txt file
			FILE* fp2; // second txt file
			char* line1 = NULL;
			char* line2 = NULL;
			size_t len1 = 0;
			ssize_t len2 = 0;
			ssize_t read1;
			ssize_t read2;
			fp1 = fopen(first_path, "r");
			fp2 = fopen(second_path, "r");

			char* bitwise1 = malloc(8);
			char* bitwise2 = malloc(8);
			int bitcount = 0;

			if (fp1 == NULL || fp2 == NULL) {
				exit(EXIT_FAILURE);
			}
			int mismatch_counter = 0;  // counter for mismatches
			int line_counter = 0;  // counts the lines
			int no_difference = 0; // checks if the files are identical

			if (strcmp(flag, "-a") == 0) {  // case we compare line by line
				while ((read1 = getline(&line1, &len1, fp1)) != -1 && (read2 = getline(&line2, &len2, fp2)) != -1) {
					line_counter++;
					if (strcmp(line1, line2) != 0) {  // checks if the linesare identical if they are not counters updated lines are printed 
						no_difference++;
						mismatch_counter++;
						printf("%s :Line %d: %s", first_txt, line_counter, line1);
						printf("%s :Line %d: %s", second_txt, line_counter, line2);
					}
				}

				if (read1 == -1 && (read2 = getline(&line2, &len2, fp2)) != -1) {  // case that first txt ends but not second txt. Printing extra lines
				//read2 = getline(&line2, &len2 , fp2); // this line is nessecary because of the && in the while loop above. 
					no_difference++;
					line_counter++;
					mismatch_counter++;
					printf("%s :Line %d: %s", second_txt, line_counter, line2);
					printf("%s :Line %d:is null \n", first_txt, line_counter);
					while ((read2 = getline(&line2, &len2, fp2)) != -1) {
						line_counter++;
						mismatch_counter++;
						printf("%s :Line %d: %s", second_txt, line_counter, line2);
						printf("%s :Line %d:is null \n", first_txt, line_counter);
					}
				}
				else if (read1 != -1 && read2 == -1) {  // case that second txt ends but not first txt. Printing extra lines
					no_difference++;
					line_counter++;
					mismatch_counter++;
					printf("%s :Line %d: %s", first_txt, line_counter, line1);
					printf("%s :Line %d:is null  \n", second_txt, line_counter);
					while ((read1 = getline(&line1, &len1, fp1)) != -1) {
						line_counter++;
						mismatch_counter++;

						printf("%s :Line %d: %s", first_txt, line_counter, line1);
						printf("%s :Line %d:is null  \n", second_txt, line_counter);
					}
				}
				if (no_difference != 0) { // if the all lines are not identical print how many lines are different
					printf("%d different line found \n", mismatch_counter);
				}
				else {
					printf("All lines are identical \n");

				}
			}
			else { // this part implements -b case which is bitwise comparison

				int char1;
				int char2;
				int bitcntr = 0;
				while ((char1 = fgetc(fp1)) != EOF && (char2 = fgetc(fp2)) != EOF) { // compares bit by bit 
					if ((char)char1 != (char)char2)bitcntr++; 	// 1 char is  1 bit
				}if (char1 == EOF) { //case when first txt file is finished
					while ((char2 = fgetc(fp2)) != EOF) {
						bitcntr++;
					}
				}if (char2 == EOF) { // case when second txt file is finished
					while ((char1 = fgetc(fp1)) != EOF) {
						bitcntr++;
					}
					if (bitcntr == 0) printf("Two files are identical \n ");
					if (bitcntr != 0)printf("Files are different in  %d bytes \n", bitcntr);
				}
			}
		}

		if (strcmp(command->name, "goodMorning") == 0) { // command Good Morning / basic idea is storing the processes that will be scheduled in a txt file. In the txt file everything must be written in  crontab format
			char* time = command->args[0];
			char* hour = strdup(strtok(time, "."));// tokenize hour
			char* minute = strdup(strtok(NULL, " ")); // tokenize minute
			//printf("time:%s,minute:%s ", hour,minute );
			FILE *fp = NULL;
			char* nameof_txt = "sched.txt";// name of txt file that will be opened at the current directory
			fp = fopen(nameof_txt, "a");
			char* current_direct = malloc(PATH_MAX);
			getcwd(current_direct, PATH_MAX);
			current_direct = realloc(current_direct, strlen(current_direct) + 10);
			strcat(current_direct, "/");
			strcat(current_direct, nameof_txt);
			int file_desc = open(current_direct, O_WRONLY | O_APPEND); // opens a schedule.txt file to store the processes that will be scheduled 

			if (file_desc < 0)
				printf("Error opening the file\n");

			// dup() will create the copy of file_desc as the copy_desc 
			// then both can be used interchangeably. 

			int copy_desc = dup(file_desc);

			// write() will write the given string into the file 
			// referred by the file descriptors 
			char* toWrite = strdup(minute); // toWrite is the string in the crontab syntax
			toWrite = realloc(toWrite, strlen(toWrite) + strlen(time) + strlen(command->args[1]) + 10);
			strcat(toWrite, " ");
			strcat(toWrite, hour);
			strcat(toWrite, " * * * ");
			strcat(toWrite, command->args[1]);
			int j = 2;
			while (command->args[j] != NULL) {
				toWrite = realloc(toWrite, strlen(toWrite) + strlen(command->args[j]) + 2);
				strcat(toWrite, " ");
				strcat(toWrite, command->args[j]);
				j++;
			}
			strcat(toWrite, "\n");

			write(copy_desc, toWrite, strlen(toWrite)); // writes the process to be scheduled to txt file which is created

			pid_t pid = fork();
			if (pid == 0) {

				char* cmd = "crontab";
				char* argcron[3];
				argcron[0] = "crontab";
				argcron[1] = current_direct;
				argcron[2] = NULL;

				execvp(cmd, argcron);// executes crontab,command to set the alarm in corontab 
				exit(0);

			}
		}


		if (strcmp(command->name, "shortdir") == 0) { // basic idea is storing all the related paths in the format SHORT_NAME>EXACT_PATH. These would be stored in a txt file. When we write the shortname and jump command then we will search for the SHORT_NAME and take the exact path from txt file.
			char* cwd = malloc(PATH_MAX);
			char* homedir = getenv("HOME");// returns to the HOME variable this program will create txt file there
			char* adress = strdup(homedir);
			adress = realloc(adress, strlen(adress) + 12);
			strcat(adress, "/Direct.txt"); // file that stores the short names and related paths in the SHORT_NAME>EXACT_PATH format

			char* interchange_adress = strdup(homedir);

			interchange_adress = realloc(interchange_adress, strlen(interchange_adress) + 13);
			strcat(interchange_adress, "/Direct2.txt");// file that will be used for clear and delete /stores the short names and related paths in the SHORT_NAME>EXACT_PATH format


			if (strcmp(command->args[0], "set") == 0) {


				FILE *f = fopen(adress, "a");
				if (f == NULL)
				{
					printf("Error opening file!\n");
					exit(1);
				}

				/* print some text */
				getcwd(cwd, PATH_MAX);
				char* toWrite = strdup(command->args[1]);
				toWrite = realloc(toWrite, strlen(toWrite) + strlen(cwd) + 2);
				strcat(toWrite, ">");
				strcat(toWrite, cwd);
				strcat(toWrite, "\n");

				fprintf(f, "%s", toWrite);

				/* print integers and floats */


				fclose(f);




				//int file = open("/home/doruk/Documents/Direct.txt", O_WRONLY | O_APPEND); // file that stores the short names and related paths in the SHORT_NAME>EXACT_PATH format

				  //  if(file < 0) 
				  //      printf("Error opening the file\n"); 

					// dup() will create the copy of file_desc as the copy_desc 
				   // int copy = dup(file); 
				 // char* toWrite = strdup(command->args[1]);
				 // toWrite=realloc(toWrite, strlen(toWrite)+strlen(cwd)+2);
				 //strcat(toWrite,">");
				 // strcat(toWrite,cwd);
				 // strcat(toWrite,"\n");
				 // write(copy,toWrite, strlen(toWrite)); // write() will write the given string into the file in SHORT_NAME>EXACT_PATH format.


			}


			if (strcmp(command->args[0], "list") == 0) {
				FILE * file;
				file = fopen(adress, "r");
				if (file != NULL) {
					char line[512];
					while (fgets(line, sizeof(line), file) != NULL) {
						printf("%s", line);
					}
					fclose(file);
				}
				else {
					printf("File could not be opened.");
					return EXIT;
				}


			}

			if (strcmp(command->args[0], "jump") == 0) { // idea is searching the SHORTNAME in the file line by line. Then tokenize the line and get the exact path
				FILE * fp;
				char * line = NULL;
				size_t len = 0;
				ssize_t read;

				fp = fopen(adress, "r");
				if (fp == NULL)
					exit(EXIT_FAILURE);

				while ((read = getline(&line, &len, fp)) != -1) {

					char* token = strtok(line, ">");
					// printf("token is -%s-,-%s- looking for", token,command->args[1]   );
					if (strcmp(command->args[1], token) == 0) {
						token = strtok(NULL, ">");

						char* directory = malloc(strlen(token) - 1);
						strncpy(directory, token, strlen(token) - 1);
						strcat(directory, "/");
						//   printf("token is %s----", directory);
						chdir(directory);
					}
				}
				fclose(fp);
				if (line)
					free(line);
			}
			if (strcmp(command->args[0], "delete") == 0) {// at delete idea is searching for the line which is going to be deleted(while searching, all the lines are written into new txt file that will be interchanged with the current txt file) than skipping that line(and keep going writing the other lines) and write all the other lines to the new txt file than change it name with old one
				FILE * fp1;
				FILE * fp2;
				char * line = NULL;
				size_t len = 0;
				ssize_t read;
				fp1 = fopen(adress, "r"); // old file
				if (fp1 == NULL) exit(EXIT_FAILURE);

				fp2 = fopen(interchange_adress, "w");

				while ((read = getline(&line, &len, fp1)) != -1) { // write all the lines but not the lines that we want to delete to the new txt
					char* clone = strdup(line);
					char* token = strtok(line, ">");
					//printf("token is %s", token);
					if (strcmp(command->args[1], token) != 0) {
						fprintf(fp2, clone);
					}
				}
				fclose(fp1);
				fclose(fp2);
				remove(adress); // remove old one 
				rename(interchange_adress, adress); // rename new txt file
			}if (strcmp(command->args[0], "clear") == 0) { // remove and open new blank txt file so everything is cleared
				remove(adress);
				FILE* fp1;
				fp1 = fopen(adress, "w");

			}
		}
		return SUCCESS;

		printf("-%s: %s: command not found\n", sysname, command->name);
		return UNKNOWN;
	}
}
