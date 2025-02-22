//
// Created by semeg on 16/07/2024.
//

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <stddef.h>
#include <getopt.h>
#include <unistd.h>

#define REQUEST_TAG 1
#define WORK_TAG 2
#define NO_WORK_TAG 3
#define SOLUTION_FOUND_TAG 4
#define TOKEN_TAG 5
#define TERMINATION_TAG 6
#define WHITE 0
#define BLACK 1


// Variabili globali per memorizzare i tempi di esecuzione delle varie fasi
double total_seq_time_handle_requests;
double total_seq_time_split_work;
double total_seq_time_request_work;
double total_seq_time_encode_stack;


int proc_state = WHITE;

// Struttura per rappresentare lo stato delle configurazioni esplorate
typedef struct {
    char **status;  // Matrice dei caratteri per lo stato delle celle
    int row;
    int col;
} State;

// Definizione di una struttura per lo stato compresso
typedef struct {
    uint64_t compressed_status;
    int row;
    int col;
} CompressedState;

int calculate_state_size(int rows) {
    return sizeof(int) * 2 + (rows * rows * (sizeof(char) + 7));  // Allinea a 8 byte
}

// Invia lo stack con stati compressi a un altro processo
void send_stack(State *stack, int top, int dest, int rows, MPI_Datatype compressed_state_type, int rank, int size) {
    double seq_time_encode_stack_start, seq_time_encode_stack_end;

    seq_time_encode_stack_start = MPI_Wtime();

    CompressedState *compressed_stack = malloc(top * sizeof(CompressedState));

    for (int i = 0; i < top; i++) {
        compressed_stack[i].compressed_status = gridToNumber(stack[i].status, rows);
        compressed_stack[i].row = stack[i].row;
        compressed_stack[i].col = stack[i].col;
    }

    if (dest < rank && !(dest == 0 && rank == size - 1))
        proc_state = BLACK;

    seq_time_encode_stack_end = MPI_Wtime();
    total_seq_time_encode_stack += (seq_time_encode_stack_end- seq_time_encode_stack_start);

    MPI_Send(compressed_stack, top, compressed_state_type, dest, WORK_TAG, MPI_COMM_WORLD);
    free(compressed_stack);
}

// Verifica se c'è abbastanza lavoro per condividerlo
bool have_enough_work_to_share(int top, int stack_cutoff, int *count, bool *sequential_work) {
    if (top < 0) {
        return false;
    }

    for (int i = 0; i <= top; i++) {
        (*count)++;
    }

    if (*count > stack_cutoff) {
        *sequential_work = false;
        return true;
    }

    return false;
}

// Divide il lavoro dallo stack per condividerlo con altri processi
State* split_work_from_stack(State **stack, int *top, int rows, int *num_states, int cutoff_index) {
    double seq_time_split_work_start, seq_time_split_work_end;

    seq_time_split_work_start = MPI_Wtime();
    if (cutoff_index > *top) {
        cutoff_index = *top / 2;
    }

    int split_point = cutoff_index / 2;
    *num_states = *top - split_point + 1;

    if (*num_states <= 0) {
        return NULL;
    }

    State* work_to_send = malloc((*num_states) * sizeof(State));
    if (work_to_send == NULL) {
        perror("malloc failed");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    for (int i = 0; i < *num_states; i++) {
        work_to_send[i] = (*stack)[split_point + i];
        work_to_send[i].status = malloc(rows * sizeof(char*));
        for (int j = 0; j < rows; j++) {
            work_to_send[i].status[j] = malloc(rows * sizeof(char));
            memcpy(work_to_send[i].status[j], (*stack)[split_point + i].status[j], rows * sizeof(char));
        }
    }

    for (int i = split_point; i <= *top; i++) {
        for (int j = 0; j < rows; j++) {
            free((*stack)[i].status[j]);
        }
        free((*stack)[i].status);
    }

    *top = split_point - 1;

    seq_time_split_work_end = MPI_Wtime();
    total_seq_time_split_work += (seq_time_split_work_end - seq_time_split_work_start);

    return work_to_send;
}

// Gestisce una richiesta di lavoro da un altro processo
void handle_work_request(int requesting_process, State **stack, int *top, int rank, int size, int rows, int stack_cutoff, MPI_Datatype compressed_state_type, bool *sequential_work) {
    int count = 0;
    double seq_time_handle_requests_start, seq_time_handle_requests_end;

    seq_time_handle_requests_start = MPI_Wtime();
    if (have_enough_work_to_share(*top, stack_cutoff, &count, sequential_work)) {
        int num_states;
        State* work_to_send = split_work_from_stack(stack, top, rows, &num_states, count);

        send_stack(work_to_send, num_states, requesting_process, rows, compressed_state_type, rank, size);

        for (int i = 0; i < num_states; i++) {
            for (int j = 0; j < rows; j++) {
                free(work_to_send[i].status[j]);
            }
            free(work_to_send[i].status);
        }
        free(work_to_send);
        seq_time_handle_requests_end = MPI_Wtime();
        total_seq_time_handle_requests += (seq_time_handle_requests_end - seq_time_handle_requests_start);
    } else {
        char no_work = 0;
        MPI_Send(&no_work, 1, MPI_CHAR, requesting_process, NO_WORK_TAG, MPI_COMM_WORLD);
    }

}

// Gestisce le richieste di lavoro in arrivo
void handle_incoming_requests(State **stack, int *top, int rank, int size, int rows, int stack_cutoff, MPI_Datatype compressed_state_type, bool *sequential_work) {
    MPI_Status status;
    int flag;
    double seq_time_handle_requests_start, seq_time_handle_requests_end;

    MPI_Iprobe(MPI_ANY_SOURCE, REQUEST_TAG, MPI_COMM_WORLD, &flag, &status);

    while (flag) {
        int source = status.MPI_SOURCE;

        char request;
        MPI_Recv(&request, 1, MPI_CHAR, source, REQUEST_TAG, MPI_COMM_WORLD, &status);

        seq_time_handle_requests_start = MPI_Wtime();
        handle_work_request(source, stack, top, rank, size, rows, stack_cutoff, compressed_state_type, sequential_work);
        seq_time_handle_requests_end = MPI_Wtime();
        total_seq_time_handle_requests += (seq_time_handle_requests_end - seq_time_handle_requests_start);

        MPI_Iprobe(MPI_ANY_SOURCE, REQUEST_TAG, MPI_COMM_WORLD, &flag, &status);
    }
}

// Richiede lavoro da altri processi quando lo stack è vuoto
bool request_work(State **stack, int *top, int rank, int size, int rows, MPI_Datatype compressed_state_type) {
    MPI_Status status;
    int stack_size = 0;
    double seq_time_request_work_start, seq_time_request_work_end;

    for (int j = 1; j < size; j++) {
        int target = (rank + j) % size;
        char request = 1;

        MPI_Send(&request, 1, MPI_CHAR, target, REQUEST_TAG, MPI_COMM_WORLD);

        int flag;
        MPI_Iprobe(target, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);

        if (flag) {
            if (status.MPI_TAG == WORK_TAG) {
                MPI_Get_count(&status, compressed_state_type, &stack_size);
                CompressedState *compressed_stack = malloc(stack_size * sizeof(CompressedState));

                MPI_Recv(compressed_stack, stack_size, compressed_state_type, target, WORK_TAG, MPI_COMM_WORLD, &status);

                seq_time_request_work_start = MPI_Wtime();

                *top = stack_size - 1;
                *stack = realloc(*stack, (*top + 1) * calculate_state_size(rows));

                for (int i = 0; i <= *top; i++) {
                    (*stack)[i].status = malloc(rows * sizeof(char*));
                    for (int r = 0; r < rows; r++) {
                        (*stack)[i].status[r] = malloc(rows * sizeof(char));
                    }
                    numberToGrid(compressed_stack[i].compressed_status, (*stack)[i].status, rows);
                    (*stack)[i].row = compressed_stack[i].row;
                    (*stack)[i].col = compressed_stack[i].col;
                }

                seq_time_request_work_end = MPI_Wtime();


                free(compressed_stack);
                total_seq_time_request_work += (seq_time_request_work_end - seq_time_request_work_start);

                return true;
            } else if (status.MPI_TAG == NO_WORK_TAG) {
                char dummy;
                MPI_Recv(&dummy, 1, MPI_CHAR, target, NO_WORK_TAG, MPI_COMM_WORLD, &status);
                continue;
            }
        }
    }

    return false;
}

// Controlla se è stata trovata una soluzione
bool check_solution_found(void) {
    MPI_Status status;
    int flag = 0;
    MPI_Iprobe(MPI_ANY_SOURCE, SOLUTION_FOUND_TAG, MPI_COMM_WORLD, &flag, &status);
    if (flag) {
        char dummy;
        MPI_Recv(&dummy, 1, MPI_CHAR, status.MPI_SOURCE, SOLUTION_FOUND_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        return true;
    }
    return false;
}

// Trasmette a tutti i processi che una soluzione è stata trovata
void broadcast_solution_found(int rank, int size) {
    char dummy = 0;
    for (int i = 0; i < size; i++) {
        if (i != rank) {
            MPI_Send(&dummy, 1, MPI_CHAR, i, SOLUTION_FOUND_TAG, MPI_COMM_WORLD);
        }
    }
}

// Trasmette a tutti i processi il messaggio di terminazione
void broadcast_termination(int rank, int size) {
    char dummy = 0;
    for (int i = 0; i < size; i++) {
        if (i != rank) {
            MPI_Send(&dummy, 1, MPI_CHAR, i, TERMINATION_TAG, MPI_COMM_WORLD);
        }
    }
}

// Controlla se è stato ricevuto il messaggio di terminazione
bool check_termination(void) {
    int flag = 0;
    MPI_Iprobe(0, TERMINATION_TAG, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
    if (flag) {
        char dummy;
        MPI_Recv(&dummy, 1, MPI_CHAR, 0, TERMINATION_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        return true;
    }
    return false;
}

// Controlla se è stato ricevuto il messaggio di terminazione
void handle_token_reception(char *token, int rank, int size) {
    int flag;
    int source;

    if (rank == 0)
        source = size - 1;
    else
        source = rank - 1;

    MPI_Iprobe(source, TOKEN_TAG, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);

    if (flag) {
        MPI_Recv(token, 1, MPI_CHAR, source, TOKEN_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
}

// Invia il token di terminazione al prossimo processo
void send_token(char *token, int rank, int size) {
    if (*token != -1) {
        if (proc_state == BLACK)
            *token = 'B'; // BLACK

        MPI_Send(token, 1, MPI_CHAR, (rank + 1) % size, TOKEN_TAG, MPI_COMM_WORLD);

        proc_state = WHITE;
        *token = -1;
    }
}


// Genera configurazioni del puzzle Hitori e verifica le soluzione (DFS parallela
double generateConfigurations(int **matrix, int rows, bool **visited, int rank, int size, int stack_cutoff, int work_chunk_size, MPI_Datatype compressed_state_type, bool benchmark_mode) {
    long count = 0;
    bool found_solution = false;
    bool terminate = false;
    const int NUMRETRY = 50;
    char token = (rank == 0) ? 'W' : -1; // Token per l'anello, inizializzato nel processo root
    bool sequential_work = true;

    State *stack = malloc(size * calculate_state_size(rows));
    int top = -1;

    double seq_start_time = 0.0, seq_end_time, seq_total_time = 0.0;

    // Inizializzazione dello stack nel processo principale
    if (rank == 0) {
        seq_start_time = MPI_Wtime();
        State initialState;
        initialState.status = malloc(rows * sizeof(char*));
        for (int i = 0; i < rows; i++) {
            initialState.status[i] = malloc(rows * sizeof(char));
            memset(initialState.status[i], '.', rows * sizeof(char));
        }
        initialState.row = 0;
        initialState.col = 0;

        stack[++top] = initialState;
    }

    // Ciclo principale per generare configurazioni e verificare soluzioni
    while (!found_solution && !terminate) {
        if (top < 0) {
            if (size == 1)
                break;
            if (rank == 0 && top < 0 && token == 'W') {
                broadcast_termination(rank, size);
                break;
            }
            //inizia la procedura di terminazione
            if (rank == 0) {
                proc_state = WHITE;
                token = 'W'; // WHITE
                send_token(&token, rank, size);
            } else {
                if (token != -1) {
                    send_token(&token, rank, size);
                }
            }

            bool received_work = false;
            for (int j = 0; j < NUMRETRY; j++) {
                received_work = request_work(&stack, &top, rank, size, rows, compressed_state_type);
                if (received_work) {
                    break;
                }
            }
        } else {
            int i = 0;
            while (i < work_chunk_size && top >= 0) {
                State currentState = stack[top--];
                int row = currentState.row;
                int col = currentState.col;

                if (row == rows) {
                    i++;
                    count++;
                    if (is_valid(matrix, currentState.status, rows, rows)) {
                        print_solution(currentState.status, rows, matrix);
                        found_solution = true;
                        printf("Process %d: Solution found\n", rank);
                    }
                    for (int j = 0; j < rows; j++) {
                        free(currentState.status[j]);
                    }
                    free(currentState.status);

                    if (found_solution) {
                        broadcast_solution_found(rank, size);
                        break;
                    }

                    continue;
                }

                int nextRow = col == rows - 1 ? row + 1 : row;
                int nextCol = col == rows - 1 ? 0 : col + 1;

                State nextState;
                nextState.status = malloc(rows * sizeof(char*));
                for (int j = 0; j < rows; j++) {
                    nextState.status[j] = malloc(rows * sizeof(char));
                    memcpy(nextState.status[j], currentState.status[j], rows * sizeof(char));
                }

                nextState.row = nextRow;
                nextState.col = nextCol;
                stack[++top] = nextState;

                if (isSafe(currentState.status, rows, rows, row, col, visited, matrix)) {
                    State nextStateX;
                    nextStateX.status = malloc(rows * sizeof(char*));
                    for (int j = 0; j < rows; j++) {
                        nextStateX.status[j] = malloc(rows * sizeof(char));
                        memcpy(nextStateX.status[j], currentState.status[j], rows * sizeof(char));
                    }
                    nextStateX.status[row][col] = 'X';
                    nextStateX.row = nextRow;
                    nextStateX.col = nextCol;
                    stack[++top] = nextStateX;
                }

                for (int j = 0; j < rows; j++) {
                    free(currentState.status[j]);
                }
                free(currentState.status);
            }

        }

        if(rank == 0 && sequential_work) {
            seq_end_time = MPI_Wtime();
            seq_total_time = seq_end_time - seq_start_time;
        }

        handle_incoming_requests(&stack, &top, rank, size, rows, stack_cutoff, compressed_state_type, &sequential_work);

        if (token == -1)
            handle_token_reception(&token, rank, size);

        if (check_solution_found())
            found_solution = true;

        if (check_termination())
            terminate = true;
    }

    for (int i = 0; i <= top; i++) {
        for (int j = 0; j < rows; j++) {
            free(stack[i].status[j]);
        }
        free(stack[i].status);
    }
    free(stack);

    if(!benchmark_mode)
        printf("Process %d: total configurations explored: %ld\n", rank, count);

    return seq_total_time;
}

// Approccio brute force per l'esplorazione delle configurazioni della griglia
void generate_solution(int **matrix, int n, int rank, int size, bool **visited, bool benchmark) {
    long max_x = pow(2, n * n);
    long chunk_size = max_x / size;
    long start = rank * chunk_size;
    long end = (rank == size - 1) ? max_x : start + chunk_size;
    int count_solutions = 0;
    int global_count = 0;

    printf("p%d; start: %ld, end: %ld\n", rank, start, end);

    char **status = malloc(n * sizeof(char*));
    for (int i = 0; i < n; i++) {
        status[i] = malloc(n * sizeof(char));
        memset(status[i], '.', n * sizeof(char));
    }

    for (long config = start; config < end; config++) {

        numberToGrid(config, status, n);
        if (is_valid_1(matrix, status, n, n) && !hasIsland(status, visited, n)) {
            printf("Process %d has found solution\n", rank);
            print_solution(status, n, matrix);
            count_solutions++;
            break;
        }
    }

    MPI_Reduce(&count_solutions, &global_count, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

    for(int i = 0; i < n; i++) {
        free(status[i]);
    }
    free(status);
    if(rank == 0 && !benchmark)
        printf("There are %d solutions for this Hitori puzzle\n", global_count);
}
