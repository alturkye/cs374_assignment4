#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>


// global variables
int last_status = 0;
int fg_only_mode = 0; 

// struct to hold parsed command data 
struct command_line{
    char *argv[513]; // MAX_ARGS + 1 for null 
    char *input_file;
    char *output_file;
    bool is_bg;
};

// signal handlers 

// sigtstp handler - toggles foreground only mode
void handle_SIGTSTP(int signo){
    if(fg_only_mode == 0){
        char* message = "\nEntering foreground only mode \n";
        write(STDOUT_FILENO, message, 49); // write if async signal safe
        fg_only_mode = 1; 
    } else {
        char* message = "\nExiting foreground only mode\n";
        write(STDOUT_FILENO, message, 29);
        fg_only_mode = 0;
    }
}


//function to parse expanded input string 
struct command_line *parse_expanded_input(char* expanded_input){
    struct command_line *curr_cmd = calloc(1, sizeof(struct command_line));
    int i = 0;

    // tokenize expanded input 
    char *token = strtok(expanded_input, " ");

    while(token != NULL){
        if(strcmp(token, "<") == 0){
            // next token is input file 
            token = strtok(NULL, " ");
            curr_cmd->input_file = strdup(token);
        }else if(strcmp(token, ">")==0){
            // next token is output file 
            token = strtok(NULL, " ");
            curr_cmd->output_file = strdup(token);
        } else {
            // normal arg
            curr_cmd->argv[i++] = token;
        }
        token = strtok(NULL, " ");
    }

    // & at the end 
    if(i > 0 && strcmp(curr_cmd->argv[i-1], "&") == 0){
        curr_cmd->is_bg = true;
        curr_cmd->argv[i-1] = NULL; 
    } else {
        curr_cmd->argv[i] = NULL; // null terminate for execvp()
    }
    return curr_cmd;
}

void handle_cd(char* path){
    if(path == NULL){
        // no arguments, change to home directory 
        chdir(getenv("HOME"));
    }else {
        // change to specified relative or absolute path 
        if (chdir(path) != 0){
            perror("cd");
        }
    }
}

char* expand_pid(char* input){
    char* result = calloc(2048, sizeof(char)); 
    char pid_str[16]; 
    sprintf(pid_str, "%d", getpid()); // get pid and convert to string 

    int j =0; 
    for(int i = 0; i < strlen(input); i++){
        // find $$, replace it with PID string 
        if(input[i] == '$' && input[i+1] == '$'){
            strcat(result, pid_str);
            j += strlen(pid_str);
            i++;    // skip the second $
        } else{
            result[j++] = input[i];
        }
    }
    return result;
}

int main(){

    // register sigtstp handler 
    struct sigaction SIGTSTP_action = {0};
    SIGTSTP_action.sa_handler = handle_SIGTSTP; 
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    // parent should ignore sigint
    struct sigaction SIGINT_action = {0};
    SIGINT_action.sa_handler = SIG_IGN;
    sigfillset(&SIGINT_action.sa_mask);
    SIGINT_action.sa_flags = 0;
    sigaction(SIGINT, &SIGINT_action, NULL);

    char input[2048]; // max command length 

    while(1){
        // zombie cleanup 
        int childStatus; 
        pid_t childPid = waitpid(-1, &childStatus, WNOHANG);
        while (childPid > 0){
            printf("background pid %d is done: ", childPid);
            if (WIFEXITED(childStatus)) 
                printf("exit value %d\n", WEXITSTATUS(childStatus));
            else 
                printf("terminated by signal %d\n", WTERMSIG(childStatus));
            fflush(stdout);
            childPid = waitpid(-1, &childStatus, WNOHANG);
        }

        printf(": ");
        fflush(stdout); // flush after every print 

        if (fgets(input, 2048, stdin) == NULL)
            break;

        input[strcspn(input, "\n")] = '\0'; 

        // expand $$
        char* expanded_input = expand_pid(input);

        // expanded for comments check + parsing
        if(strlen(expanded_input) == 0 || expanded_input[0] == '#'){
            free(expanded_input);
            continue;
        }

        struct command_line *cmd = parse_expanded_input(expanded_input);

        // execute builtin commands 
        if(strcmp(cmd->argv[0], "exit") == 0){
            // kill other processes before terminating 
            kill(0, SIGTERM);
            exit(0);
        }
        else if (strcmp(cmd->argv[0], "cd") == 0){
            handle_cd(cmd->argv[1]);
        }
        else if
            (strcmp(cmd->argv[0], "status") == 0){
                // use macros to interpret the termination status 
                if (WIFEXITED(last_status)){
                    // normal exit 
                    printf("exit value %d\n", WEXITSTATUS(last_status));
                } else {
                    // terminaton by a singal 
                    printf("terminated by signal %d\n", 
                                WTERMSIG(last_status));
                }
                fflush(stdout);
        }else {
            // fork() + exec() 
            pid_t spawnPid = fork(); 
            if(spawnPid == 0){
                // handle sigint
                struct sigaction SIGINT_action = {0};
                SIGINT_action.sa_handler = SIG_DFL; //change bck to default
                sigaction(SIGINT, &SIGINT_action, NULL);

                // badfile case 
                if(cmd->input_file){
                    int fd = open(cmd->input_file, O_RDONLY);
                    if(fd == -1){
                        perror("cannot open file");
                        exit(1); 
                    }
                    dup2(fd, 0); 
                    close(fd); 
                }
                //default background input to /dev/null
                else if(cmd->is_bg && !fg_only_mode){
                    int fd = open("/dev/null", O_RDONLY);
                    dup2(fd, 0);
                    close(fd);
                }
                // output redirection
                if (cmd->output_file){
                    int fd = open(cmd->output_file, O_WRONLY | O_CREAT |
                            O_TRUNC, 0644);
                    if (fd == -1){
                        perror("cannot open file");
                        exit(1);
                    }
                    dup2(fd, 1);
                    close(fd);
                }
                // default background ooutpuut to /dev/null
                else if (cmd->is_bg && !fg_only_mode){
                    int fd = open("/dev/null", O_WRONLY);
                    dup2(fd, 1);
                    close(fd);
                }

                // run program
                execvp(cmd->argv[0], cmd->argv);
                perror(cmd->argv[0]);
                exit(1);
            } else {
                // in parent, wait if foreground mode is on
                if(!cmd->is_bg || fg_only_mode){
                    waitpid(spawnPid, &last_status, 0);
                } else {
                    printf("background pid is %d\n", spawnPid);
                    fflush(stdout);
                }
            }

        }


        // memory cleanup 
        free(cmd->input_file);
        free(cmd->output_file);
        free(cmd);
        free(expanded_input);
    }

    return 0;
}
