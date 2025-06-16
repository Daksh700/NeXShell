#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdlib.h>

#define MAXHISTORY 100
#define MAXINPUT 100

int main()
{
    char input[100];
    char history[MAXHISTORY][MAXINPUT];
    int historyCount = 0;

    // Using space as delimiter to tokenize the command string
    char delim[] = " ";
    char *args[10];

    struct timeval start, end;

    // Maintain a whitelist of supported commands for validation and suggestions
    const char *valid_commands[] = {"cd", "exit", "clear", "history", "ls", "pwd", NULL};

    // Map user-friendly phrases to actual shell commands (rudimentary NLP support)
    char *nl_phrases[][2] = {
        {"show files", "ls"},
        {"list text files", "ls *.txt"},
        {"current directory", "pwd"},
        {"who am i", "whoami"},
        {"show processes", "ps"},
        {"clear screen", "clear"},
        {NULL, NULL}};

    while (1)
    {
        printf("NeXShell> ");
        fgets(input, MAXINPUT, stdin);
        input[strcspn(input, "\n")] = 0; // Remove newline to sanitize input

        // Keep a record of every command typed for the history feature
        if (historyCount < MAXHISTORY)
        {
            strcpy(history[historyCount], input);
            historyCount++;
        }
        else
        {
            // Avoid overflow of fixed-size history buffer
            printf("History buffer full\n");
        }

        // Exit loop if user types 'exit'
        if (strcmp("exit", input) == 0)
        {
            break;
        }

        int matches = 0;
        int m;

        // Translate natural language input to actual command if matched
        for (m = 0; nl_phrases[m][0] != NULL; m++)
        {
            if (strcmp(input, nl_phrases[m][0]) == 0)
            {
                matches = 1;
                break;
            }
        }

        // Replace user-friendly input with its mapped shell command
        if (matches == 1)
        {
            strcpy(input, nl_phrases[m][1]);
        }

        int background = 0;

        // Split input into arguments for execvp usage
        char *token = strtok(input, delim);
        int i = 0;
        while (token != NULL)
        {
            args[i++] = token;
            token = strtok(NULL, delim);
        }
        args[i] = NULL;

        // Validate whether the command is recognized before proceeding
        int isValid = 0;
        for (int i = 0; valid_commands[i] != NULL; i++)
        {
            if (strcmp(args[0], valid_commands[i]) == 0)
            {
                isValid = 1;
                break;
            }
        }

        // If command is invalid, provide a closest-match suggestion or report as unknown
        if (!isValid)
        {
            int suggestionPrinted = 0;
            for (int i = 0; valid_commands[i] != NULL; i++)
            {
                if (strncmp(valid_commands[i], args[0], strlen(args[0])) == 0)
                {
                    printf("Command not found: %s\n", args[0]);
                    printf("Did you mean: %s?\n", valid_commands[i]);
                    suggestionPrinted = 1;
                    break;
                }
            }
            if (!suggestionPrinted)
            {
                printf("Unknown command: %s\n", args[0]);
            }
            continue;
        }

        // Handle screen clearing immediately to avoid forking
        if (strcmp(args[0], "clear") == 0)
        {
            system("clear");
            continue;
        }

        // Detect if command should run in background
        if (i > 0 && strcmp(args[i - 1], "&") == 0)
        {
            background = 1;
            args[i - 1] = NULL; // Remove '&' before passing to execvp
        }

        if (args[0] == NULL)
        {
            continue;
        }

        // Handle shell's own history display internally
        if (strcmp(args[0], "history") == 0)
        {
            for (int i = 0; i < historyCount; i++)
            {
                printf("%d. %s\n", i + 1, history[i]);
            }
            continue;
        }

        // Change directory is a shell built-in, so it must be handled without forking
        if (strcmp(args[0], "cd") == 0)
        {
            if (args[1] == NULL)
            {
                printf("Expected argument to cd\n");
            }
            else
            {
                chdir(args[1]);
            }
        }
        else
        {
            // Check for presence of a pipe and split the command accordingly
            int pipeIndex = -1;
            for (int i = 0; args[i] != NULL; i++)
            {
                if (strcmp(args[i], "|") == 0)
                {
                    pipeIndex = i;
                    break;
                }
            }

            if (pipeIndex != -1)
            {
                // Handle piping by splitting into two commands and connecting them
                char *command1_args[10];
                char *command2_args[10];

                for (int i = 0; i < pipeIndex; i++)
                {
                    command1_args[i] = args[i];
                }
                command1_args[pipeIndex] = NULL;

                int j = 0;
                for (int i = pipeIndex + 1; args[i] != NULL; i++)
                {
                    command2_args[j++] = args[i];
                }
                command2_args[j] = NULL;

                int pipefd[2];
                if (pipe(pipefd) == -1)
                {
                    perror("Pipe Failed");
                    continue;
                }

                // First child writes to pipe
                pid_t pid1 = fork();
                if (pid1 == 0)
                {
                    close(pipefd[0]);
                    dup2(pipefd[1], STDOUT_FILENO);
                    close(pipefd[1]);
                    execvp(command1_args[0], command1_args);
                    perror("execvp failed for cmd1");
                    _exit(1);
                }

                // Second child reads from pipe
                pid_t pid2 = fork();
                if (pid2 == 0)
                {
                    close(pipefd[1]);
                    dup2(pipefd[0], STDIN_FILENO);
                    close(pipefd[0]);
                    execvp(command2_args[0], command2_args);
                    perror("execvp failed for cmd2");
                    _exit(1);
                }

                // Parent closes both ends and waits for both children
                close(pipefd[0]);
                close(pipefd[1]);
                wait(NULL);
                wait(NULL);
            }
            else
            {
                // For all other external commands, record start time for performance measurement
                gettimeofday(&start, NULL);
                pid_t pid = fork();

                if (pid == 0)
                {
                    // Inside child: check for any I/O redirection symbols
                    int outputRedirect = 0;
                    int inputRedirect = 0;
                    int appendOutputRedirect = 0;
                    int errorRedirect = 0;
                    int j = 0;
                    char *outputFilename = NULL;
                    char *inputFilename = NULL;
                    char *command_args[10];

                    for (int i = 0; args[i] != NULL; i++)
                    {
                        // Multiple redirection types supported, so check them all
                        if (strcmp(args[i], ">") == 0)
                        {
                            outputRedirect = 1;
                            outputFilename = args[i + 1];
                            i++;
                        }
                        else if (strcmp(args[i], "<") == 0)
                        {
                            inputRedirect = 1;
                            inputFilename = args[i + 1];
                            i++;
                        }
                        else if (strcmp(args[i], ">>") == 0)
                        {
                            appendOutputRedirect = 1;
                            outputFilename = args[i + 1];
                            i++;
                        }
                        else if (strcmp(args[i], "2>") == 0)
                        {
                            errorRedirect = 1;
                            outputFilename = args[i + 1];
                            i++;
                        }
                        else
                        {
                            command_args[j++] = args[i];
                        }
                    }

                    command_args[j] = NULL;

                    // Redirect stdout to file if needed
                    if (outputRedirect)
                    {
                        int fd = open(outputFilename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (fd < 0)
                        {
                            perror("Open Failed");
                            _exit(1);
                        }
                        dup2(fd, STDOUT_FILENO);
                        close(fd);
                    }

                    // Append mode for output
                    if (appendOutputRedirect)
                    {
                        int fd = open(outputFilename, O_WRONLY | O_CREAT | O_APPEND, 0644);
                        if (fd < 0)
                        {
                            perror("Open Failed");
                            _exit(1);
                        }
                        dup2(fd, STDOUT_FILENO);
                        close(fd);
                    }

                    // Redirect input from file
                    if (inputRedirect)
                    {
                        int fd2 = open(inputFilename, O_RDONLY);
                        if (fd2 < 0)
                        {
                            perror("Open Failed");
                            _exit(1);
                        }
                        dup2(fd2, STDIN_FILENO);
                        close(fd2);
                    }

                    // Redirect stderr
                    if (errorRedirect)
                    {
                        int fd = open(outputFilename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (fd < 0)
                        {
                            perror("Open Failed");
                            _exit(1);
                        }
                        dup2(fd, STDERR_FILENO);
                        close(fd);
                    }

                    // Execute the command finally
                    execvp(command_args[0], command_args);
                    perror("execvp failed");
                    _exit(1);
                }
                else
                {
                    // Background processes are not waited on
                    if (background)
                    {
                        printf("Background process running with PID %d\n", pid);
                    }
                    else
                    {
                        wait(NULL);
                    }
                }
            }

            // Record end time and report duration for the user
            gettimeofday(&end, NULL);
            double duration = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
            printf("Command executed in %lf\n", duration);
        }

        // Show the actual command interpreted (including mapped phrases)
        printf("You entered: %s\n", input);
    }
}