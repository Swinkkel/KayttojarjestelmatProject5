#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <math.h>

// Structure to deliver data for thread.
typedef struct {
	int thread_id;
	char* start;
	size_t size;
} thread_data_t;

// Linked list structure to store data processed by threads.
typedef struct linked_list_s {
	char c;
	unsigned int nr;
	struct linked_list_s* next;
} linked_list_t;

/* Function to do RLE for given stream.
*/
static void* process_file(void* arg) {

	thread_data_t* data = (thread_data_t*) arg;

	linked_list_t* root = malloc(sizeof(linked_list_t));
	if (root == NULL) {
		return NULL;
	}

	root->c = data->start[0];
	root->nr = 1;
	root->next = NULL;

	// If we have only one character then special handling for that case.
	if (data->size == 1) {
		return root;
	}

	linked_list_t* current_item = root;

	// Read characters from stream.
	for (int i=1;i<data->size;++i) {
		if (i == data->size || data->start[i] != data->start[i-1]) {
			// If last character or character is different than the previous one then
			// reserve new linked list item and add it to linked list.
			if (i == data->size) {
				break;
			}

			// Reserve memory for next linked list item and set pointer to it.
			linked_list_t* next = malloc(sizeof(linked_list_t));
			if (next == NULL) {
				return NULL;
			}

			next->nr = 1;
			next->c = data->start[i];
			next->next = NULL;
		
			current_item->next = next;
			current_item = next;
		}
		else {
			++current_item->nr;
		}
	}

	return root;
}

int main(int argc, char*argv[]) {
	// Check argument count.
	if (argc < 2) {
		printf("pzip: file1 [file2 ...]\n");
		return 1;
	}

	// Get number of available cores.
	int nr_cores = get_nprocs_conf();

	// Linked list to store packing information.
	linked_list_t* combined = NULL;

	for (int i=1;i<argc;++i) {
		// Open file in read mode.
		FILE* file_ptr = fopen(argv[i], "r");
		if (file_ptr == NULL) {
			fprintf(stdout, "pzip: cannot open file\n");
			exit(1);
		}

		// Get file descriptor from FILE pointer.
		int fd = fileno(file_ptr);
		if (fd == -1) {
			perror("Failed to get file descriptor");
			fclose(file_ptr);
			exit(-1);
		}

		// Get file status like size of file. Needed for memory mapped file.
		struct stat file_info;
		if (fstat(fd, &file_info) == -1) {
			perror("Failed to get file status");
			fclose(file_ptr);
			exit(-1);
		}

		// Create memory mapped file
		char* data = mmap(NULL, file_info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (data == MAP_FAILED) {
			perror("Failed to create mmap");
			fclose(file_ptr);
			exit(-1);
		}

		// Get the number of threads needed.
		int nr_threads = file_info.st_size < nr_cores ? file_info.st_size : nr_cores;

		pthread_t threads[nr_threads];
		size_t part_size = file_info.st_size / nr_threads;
		thread_data_t** thread_data = (thread_data_t**)malloc(nr_threads * sizeof(thread_data_t*));

		for (size_t i=0;i<nr_threads;++i) {
			// Reserve memory for thread data
			thread_data[i] = malloc(sizeof(thread_data_t));
			if (thread_data[i] == NULL) {
				perror("Failed to reserve memory");
				munmap(data, file_info.st_size);
				fclose(file_ptr);
				exit(-1);
			}

			thread_data[i]->thread_id = i;
			thread_data[i]->start = data + i * part_size;
			thread_data[i]->size = (i == nr_threads -1) ? file_info.st_size - i * part_size : part_size;

			// Create thread and pass the info what do process for thread.
			if (pthread_create(&threads[i], NULL, process_file, thread_data[i])) {
				perror("Failed to create thread");
				munmap(data, file_info.st_size);
				fclose(file_ptr);
				exit(-1);
			}
		}

		// Now wait that threads do their work.
		for (size_t i=0;i < nr_threads;++i) {
			linked_list_t *thread_result = NULL;
			if (pthread_join(threads[i], (void**)&thread_result)) {
				perror("Failed to join thread");
				munmap(data, file_info.st_size);
				fclose(file_ptr);
				exit(-1);
			}

			if (thread_result != NULL) {
				if (combined == NULL) {
					combined = thread_result;
				}
				else {
					linked_list_t* last = combined;
					while (last != NULL) {
						if (last->next == NULL) {
							break;
						}
						last = last->next;
					}

					if (last->c == thread_result->c) {
						last->nr += thread_result->nr;
						last->next = thread_result->next;
						free(thread_result);
					}
					else {
						last->next = thread_result;
					}
				}
			}

			free(thread_data[i]);
		}

		free(thread_data);

		// Clean up
		munmap(data, file_info.st_size);
		fclose(file_ptr);
	}

	// Go through combined result.
	linked_list_t* tmp = combined;
	while (tmp != NULL) {
		fwrite(&tmp->nr, sizeof(int), 1, stdout);
		fwrite(&tmp->c, sizeof(char), 1, stdout);

		// Free memory.
		linked_list_t* free_this = tmp;

		tmp = tmp->next;

		free(free_this);
	}

	return 0;
}
