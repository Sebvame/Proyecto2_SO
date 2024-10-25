#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>

#define BLOCK_SIZE 262144 // 256K
#define MAX_FILES 250
#define MAX_FILENAME_LENGTH 256

typedef struct
{
    char filename[MAX_FILENAME_LENGTH];
    off_t size;
    int start_block;
} FileEntry;

typedef struct
{
    int next_block;
    unsigned char data[BLOCK_SIZE - sizeof(int)];
} DataBlock;

typedef struct
{
    FileEntry files[MAX_FILES];
    int file_count;
    int free_block_list;
} StarHeader;

// Prototipos de funciones
void create_star(char *star_filename, int argc, char *argv[]);
void extract_star(char *star_filename);
void list_star(char *star_filename);
void delete_star(char *star_filename, int argc, char *argv[]);
void append_star(char *star_filename, int argc, char *argv[]);
void update_star(char *star_filename, int argc, char *argv[]);
void pack_star(char *star_filename);
void verbose_print(const char *message, int level);
int find_file_entry(StarHeader *header, char *filename);
void read_header(int fd, StarHeader *header);
void write_header(int fd, StarHeader *header);
void add_file_to_star(int fd, StarHeader *header, char *filename);
void remove_file_from_star(int fd, StarHeader *header, char *filename);
void check_file_exists(char *filename);

typedef struct
{
    int old_block;
    int new_block;
} BlockMapping;

int verbose_level = 0;

int compare_blocks(const void *a, const void *b)
{
    BlockMapping *blockA = (BlockMapping *)a;
    BlockMapping *blockB = (BlockMapping *)b;
    return blockB->old_block - blockA->old_block;
}

void check_file_exists(char *filename)
{
    struct stat buffer;
    if (stat(filename, &buffer) != 0)
    {
        fprintf(stderr, "Error: El archivo '%s' no existe\n", filename);
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Uso: star <opciones> <archivoSalida> [archivos...]\n");
        exit(EXIT_FAILURE);
    }

    int opt;
    int c_flag = 0, x_flag = 0, t_flag = 0, delete_flag = 0;
    int u_flag = 0, r_flag = 0, p_flag = 0;
    char *star_filename = NULL;

    struct option long_options[] = {
        {"create", no_argument, 0, 'c'},
        {"extract", no_argument, 0, 'x'},
        {"list", no_argument, 0, 't'},
        {"delete", no_argument, 0, 1000},
        {"update", no_argument, 0, 'u'},
        {"verbose", no_argument, 0, 'v'},
        {"file", required_argument, 0, 'f'},
        {"append", no_argument, 0, 'r'},
        {"pack", no_argument, 0, 'p'},
        {0, 0, 0, 0}};

    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "cxtrupvf:", long_options, &option_index)) != -1)
    {
        switch (opt)
        {
        case 'c':
            c_flag = 1;
            break;
        case 'x':
            x_flag = 1;
            break;
        case 't':
            t_flag = 1;
            break;
        case 'v':
            verbose_level++;
            break;
        case 'f':
            star_filename = optarg;
            break;
        case 'r':
            r_flag = 1;
            break;
        case 'u':
            u_flag = 1;
            break;
        case 'p':
            p_flag = 1;
            break;
        case 1000:
            delete_flag = 1;
            break;
        default:
            fprintf(stderr, "Opción desconocida o uso incorrecto\n");
            exit(EXIT_FAILURE);
        }
    }

    if (!star_filename)
    {
        fprintf(stderr, "Debe especificar un archivo de salida con -f o --file\n");
        exit(EXIT_FAILURE);
    }

    int operation_count = c_flag + x_flag + t_flag + delete_flag + r_flag + u_flag + p_flag;
    if (operation_count != 1)
    {
        fprintf(stderr, "Debe especificar exactamente una operación principal\n");
        exit(EXIT_FAILURE);
    }

    if (c_flag)
    {
        if (optind >= argc)
        {
            fprintf(stderr, "Debe especificar al menos un archivo para empaquetar\n");
            exit(EXIT_FAILURE);
        }
        create_star(star_filename, argc - optind, &argv[optind]);
    }
    else if (x_flag)
    {
        extract_star(star_filename);
    }
    else if (t_flag)
    {
        list_star(star_filename);
    }
    else if (r_flag)
    {
        if (optind >= argc)
        {
            fprintf(stderr, "Debe especificar al menos un archivo para agregar\n");
            exit(EXIT_FAILURE);
        }
        append_star(star_filename, argc - optind, &argv[optind]);
    }
    else if (u_flag)
    {
        if (optind >= argc)
        {
            fprintf(stderr, "Debe especificar al menos un archivo para actualizar\n");
            exit(EXIT_FAILURE);
        }
        update_star(star_filename, argc - optind, &argv[optind]);
    }
    else if (p_flag)
    {
        pack_star(star_filename);
    }
    else if (delete_flag)
    {
        if (optind >= argc)
        {
            fprintf(stderr, "Debe especificar al menos un archivo para eliminar\n");
            exit(EXIT_FAILURE);
        }
        delete_star(star_filename, argc - optind, &argv[optind]);
    }

    return 0;
}

void create_star(char *star_filename, int file_count, char *files[])
{
    // Verificar existencia de archivos antes de crear
    for (int i = 0; i < file_count; i++)
    {
        check_file_exists(files[i]);
    }

    int fd = open(star_filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0)
    {
        perror("Error al crear el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    StarHeader header;
    memset(&header, 0, sizeof(StarHeader));
    header.file_count = 0;
    header.free_block_list = -1;

    write_header(fd, &header);

    for (int i = 0; i < file_count; i++)
    {
        add_file_to_star(fd, &header, files[i]);
        if (verbose_level >= 2)
        {
            printf("Archivo '%s' agregado:\n", files[i]);
            printf("  Tamaño: %lld bytes\n", (long long)header.files[header.file_count - 1].size);
            printf("  Bloque inicial: %d\n", header.files[header.file_count - 1].start_block);
        }
    }

    write_header(fd, &header);
    close(fd);
}

void extract_star(char *star_filename)
{
    int fd = open(star_filename, O_RDONLY);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    StarHeader header;
    read_header(fd, &header);

    for (int i = 0; i < header.file_count; i++)
    {
        verbose_print("Extrayendo:", 1);
        if (verbose_level >= 1)
        {
            printf(" %s\n", header.files[i].filename);
        }

        int file_fd = open(header.files[i].filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (file_fd < 0)
        {
            perror("Error al crear archivo de salida");
            close(fd);
            exit(EXIT_FAILURE);
        }

        int current_block = header.files[i].start_block;
        off_t remaining_size = header.files[i].size;

        while (remaining_size > 0 && current_block != -1)
        {
            DataBlock block;
            lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
            if (read(fd, &block, sizeof(DataBlock)) != sizeof(DataBlock))
            {
                perror("Error al leer bloque de datos");
                close(file_fd);
                close(fd);
                exit(EXIT_FAILURE);
            }

            size_t bytes_to_write = remaining_size < sizeof(block.data) ? remaining_size : sizeof(block.data);

            if (write(file_fd, block.data, bytes_to_write) != bytes_to_write)
            {
                perror("Error al escribir datos");
                close(file_fd);
                close(fd);
                exit(EXIT_FAILURE);
            }

            remaining_size -= bytes_to_write;
            current_block = block.next_block;
        }

        close(file_fd);

        if (verbose_level >= 2)
        {
            printf("  Tamaño: %lld bytes\n", (long long)header.files[i].size);
            printf("  Bloques extraídos: %d\n",
                   (int)((header.files[i].size + sizeof(DataBlock) - 1) / sizeof(DataBlock)));
        }
    }

    close(fd);
}

void list_star(char *star_filename)
{
    int fd = open(star_filename, O_RDONLY);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    StarHeader header;
    read_header(fd, &header);

    printf("Contenido de '%s':\n", star_filename);
    for (int i = 0; i < header.file_count; i++)
    {
        printf("%s", header.files[i].filename);
        if (verbose_level >= 1)
        {
            printf(" (tamaño: %lld bytes)", (long long)header.files[i].size);
        }
        printf("\n");

        if (verbose_level >= 2)
        {
            int blocks = (header.files[i].size + sizeof(DataBlock) - 1) / sizeof(DataBlock);
            printf("  Bloques: %d\n", blocks);
            printf("  Bloque inicial: %d\n", header.files[i].start_block);
        }
    }

    close(fd);
}

void delete_star(char *star_filename, int file_count, char *files[])
{
    int fd = open(star_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    StarHeader header;
    read_header(fd, &header);

    for (int i = 0; i < file_count; i++)
    {
        int index = find_file_entry(&header, files[i]);
        if (index != -1)
        {
            remove_file_from_star(fd, &header, files[i]);

            // Mensaje consolidado de eliminación
            char message[300];
            snprintf(message, sizeof(message), "Archivo '%s' eliminado del empaquetado.", files[i]);
            verbose_print(message, 1);
        }
        else
        {
            fprintf(stderr, "El archivo '%s' no se encontró en el empaquetado.\n", files[i]);
        }
    }

    write_header(fd, &header);
    close(fd);
}

void append_star(char *star_filename, int file_count, char *files[])
{
    int fd = open(star_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    StarHeader header;
    read_header(fd, &header);

    for (int i = 0; i < file_count; i++)
    {
        if (header.file_count >= MAX_FILES)
        {
            fprintf(stderr, "Se alcanzó el número máximo de archivos en el empaquetado.\n");
            close(fd);
            exit(EXIT_FAILURE);
        }
        if (find_file_entry(&header, files[i]) != -1)
        {
            fprintf(stderr, "El archivo '%s' ya existe en el empaquetado. Use la opción -u para actualizarlo.\n", files[i]);
            continue;
        }
        add_file_to_star(fd, &header, files[i]);
    }

    write_header(fd, &header);
    close(fd);
}

void update_star(char *star_filename, int file_count, char *files[])
{
    int fd = open(star_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    StarHeader header;
    read_header(fd, &header);

    for (int i = 0; i < file_count; i++)
    {
        int index = find_file_entry(&header, files[i]);
        if (index != -1)
        {
            remove_file_from_star(fd, &header, files[i]);
            add_file_to_star(fd, &header, files[i]);
        }
        else
        {
            fprintf(stderr, "El archivo '%s' no existe en el empaquetado. Use la opción -r para agregarlo.\n", files[i]);
        }
    }

    write_header(fd, &header);
    close(fd);
}

// Agregar truncado al archivo después de la desfragmentación
void pack_star(char *star_filename)
{
    int fd = open(star_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado para lectura/escritura");
        exit(EXIT_FAILURE);
    }

    StarHeader header;
    read_header(fd, &header);

    // Recolectar todos los bloques utilizados y crear un mapeo de bloques
    int block_mapping_capacity = 1000;
    int block_mapping_count = 0;
    BlockMapping *block_mappings = malloc(block_mapping_capacity * sizeof(BlockMapping));
    if (!block_mappings)
    {
        perror("Error al asignar memoria");
        close(fd);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < header.file_count; i++)
    {
        int current_block = header.files[i].start_block;
        while (current_block != -1)
        {
            if (block_mapping_count >= block_mapping_capacity)
            {
                block_mapping_capacity *= 2;
                block_mappings = realloc(block_mappings, block_mapping_capacity * sizeof(BlockMapping));
                if (!block_mappings)
                {
                    perror("Error al asignar memoria");
                    close(fd);
                    exit(EXIT_FAILURE);
                }
            }
            block_mappings[block_mapping_count].old_block = current_block;
            block_mappings[block_mapping_count].new_block = -1;
            block_mapping_count++;

            lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
            DataBlock block;
            read(fd, &block, sizeof(DataBlock));

            current_block = block.next_block;
        }
    }

    // Ordenar los bloques en orden decreciente
    qsort(block_mappings, block_mapping_count, sizeof(BlockMapping), compare_blocks);

    // Asignar nuevos números de bloque
    int new_block_index = (sizeof(StarHeader) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (int i = block_mapping_count - 1; i >= 0; i--)
    {
        block_mappings[i].new_block = new_block_index++;
    }

    // Mover bloques y actualizar referencias
    for (int i = 0; i < block_mapping_count; i++)
    {
        int old_block = block_mappings[i].old_block;
        int new_block = block_mappings[i].new_block;

        // Leer bloque de la posición antigua
        lseek(fd, old_block * BLOCK_SIZE, SEEK_SET);
        DataBlock block;
        read(fd, &block, sizeof(DataBlock));

        // Actualizar next_block al nuevo número de bloque
        if (block.next_block != -1)
        {
            // Encontrar el nuevo número de bloque para block.next_block
            for (int j = 0; j < block_mapping_count; j++)
            {
                if (block_mappings[j].old_block == block.next_block)
                {
                    block.next_block = block_mappings[j].new_block;
                    break;
                }
            }
        }

        // Escribir bloque en la nueva posición
        lseek(fd, new_block * BLOCK_SIZE, SEEK_SET);
        write(fd, &block, sizeof(DataBlock));
    }

    // Actualizar start_block de cada archivo en el encabezado
    for (int i = 0; i < header.file_count; i++)
    {
        int old_start_block = header.files[i].start_block;
        for (int j = 0; j < block_mapping_count; j++)
        {
            if (block_mappings[j].old_block == old_start_block)
            {
                header.files[i].start_block = block_mappings[j].new_block;
                break;
            }
        }
    }

    // No hay bloques libres después de la desfragmentación
    header.free_block_list = -1;

    // Escribir encabezado actualizado
    write_header(fd, &header);

    // Truncar el archivo al nuevo tamaño
    off_t new_size = new_block_index * BLOCK_SIZE;
    ftruncate(fd, new_size); // Truncar archivo

    close(fd);
    free(block_mappings);

    if (verbose_level > 0)
    {
        printf("Archivo '%s' desfragmentado.\n", star_filename);
    }
}

void add_file_to_star(int fd, StarHeader *header, char *filename)
{
    if (header->file_count >= MAX_FILES)
    {
        fprintf(stderr, "Se alcanzó el número máximo de archivos en el empaquetado.\n");
        exit(EXIT_FAILURE);
    }

    strncpy(header->files[header->file_count].filename, filename, MAX_FILENAME_LENGTH);
    header->files[header->file_count].filename[MAX_FILENAME_LENGTH - 1] = '\0'; // Asegurar terminación

    int file_fd = open(filename, O_RDONLY);
    if (file_fd < 0)
    {
        perror("Error al abrir archivo de entrada");
        exit(EXIT_FAILURE);
    }

    struct stat st;
    fstat(file_fd, &st);
    header->files[header->file_count].size = st.st_size;

    int start_block = -1;
    int prev_block_index = -1;
    off_t remaining_size = st.st_size;

    while (remaining_size > 0)
    {
        int current_block;
        if (header->free_block_list != -1)
        {
            current_block = header->free_block_list;
            DataBlock free_block;
            lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
            read(fd, &free_block, sizeof(DataBlock));
            header->free_block_list = free_block.next_block;
        }
        else
        {
            current_block = lseek(fd, 0, SEEK_END) / BLOCK_SIZE;
        }

        if (start_block == -1)
        {
            start_block = current_block;
        }

        DataBlock block;
        memset(&block, 0, sizeof(DataBlock));
        ssize_t bytes_read = read(file_fd, block.data, sizeof(block.data));
        if (bytes_read < 0)
        {
            perror("Error al leer el archivo");
            close(file_fd);
            exit(EXIT_FAILURE);
        }

        block.next_block = -1;

        lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
        write(fd, &block, sizeof(DataBlock));

        if (prev_block_index != -1)
        {
            lseek(fd, prev_block_index * BLOCK_SIZE, SEEK_SET);
            DataBlock prev_block;
            read(fd, &prev_block, sizeof(DataBlock));
            prev_block.next_block = current_block;
            lseek(fd, prev_block_index * BLOCK_SIZE, SEEK_SET);
            write(fd, &prev_block, sizeof(DataBlock));
        }

        prev_block_index = current_block;
        remaining_size -= bytes_read;
    }

    header->files[header->file_count].start_block = start_block;
    header->file_count++;

    close(file_fd);

    // Consolidación del mensaje verbose
    char message[300];
    snprintf(message, sizeof(message), "Archivo '%s' agregado al empaquetado.", filename);
    verbose_print(message, 1);
}

void remove_file_from_star(int fd, StarHeader *header, char *filename)
{
    int index = find_file_entry(header, filename);
    if (index == -1)
    {
        fprintf(stderr, "El archivo '%s' no se encontró en el empaquetado.\n", filename);
        return;
    }

    int current_block = header->files[index].start_block;
    while (current_block != -1)
    {
        DataBlock block;
        lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
        read(fd, &block, sizeof(DataBlock));

        int next_block = block.next_block;

        block.next_block = header->free_block_list;
        header->free_block_list = current_block;

        lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
        write(fd, &block, sizeof(DataBlock));

        current_block = next_block;
    }

    for (int j = index; j < header->file_count - 1; j++)
    {
        header->files[j] = header->files[j + 1];
    }
    header->file_count--;
}

void verbose_print(const char *message, int level)
{
    if (verbose_level >= level)
    {
        printf("%s\n", message);
    }
}

int find_file_entry(StarHeader *header, char *filename)
{
    for (int i = 0; i < header->file_count; i++)
    {
        if (strcmp(header->files[i].filename, filename) == 0)
        {
            return i;
        }
    }
    return -1;
}

void read_header(int fd, StarHeader *header)
{
    lseek(fd, 0, SEEK_SET);
    read(fd, header, sizeof(StarHeader));
}

void write_header(int fd, StarHeader *header)
{
    lseek(fd, 0, SEEK_SET);
    write(fd, header, sizeof(StarHeader));
}
