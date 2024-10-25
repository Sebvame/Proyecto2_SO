#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>

#define BLOCK_SIZE 262144       // Tamaño del bloque: 256K
#define MAX_FILES 250           // Máximo número de archivos en el archivo
#define MAX_FILENAME_LENGTH 256 // Longitud máxima para nombres de archivo

// Estructura que representa una entrada de archivo en el archivador
typedef struct
{
    char filename[MAX_FILENAME_LENGTH]; // Nombre del archivo
    off_t size;                         // Tamaño del archivo en bytes
    int start_block;                    // Índice del primer bloque de datos del archivo
} FileEntry;

// Estructura que representa un bloque de datos en el archivador
typedef struct
{
    int next_block;                               // Índice del siguiente bloque de datos (-1 si es el último)
    unsigned char data[BLOCK_SIZE - sizeof(int)]; // Datos del bloque
} DataBlock;

// Estructura de encabezado para el archivador
typedef struct
{
    FileEntry files[MAX_FILES]; // Array de entradas de archivo
    int file_count;             // Número de archivos en el archivador
    int free_block_list;        // Cabeza de la lista de bloques libres (-1 si no hay)
} StarHeader;

// Estructura para mapear índices de bloques antiguos a nuevos durante la desfragmentación
typedef struct
{
    int old_block; // Índice de bloque original
    int new_block; // Índice de bloque nuevo después de la desfragmentación
} BlockMapping;

// Variable global para el nivel de verbosidad
int verbose_level = 0;

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

// Función para comparar dos mapeos de bloques (usada en qsort)
int compare_blocks(const void *a, const void *b)
{
    BlockMapping *blockA = (BlockMapping *)a;
    BlockMapping *blockB = (BlockMapping *)b;
    return blockB->old_block - blockA->old_block;
}

// Función para verificar si un archivo existe; sale del programa si no existe
void check_file_exists(char *filename)
{
    struct stat buffer;
    if (stat(filename, &buffer) != 0)
    {
        fprintf(stderr, "Error: El archivo '%s' no existe\n", filename);
        exit(EXIT_FAILURE);
    }
}

// Función principal: analiza los argumentos de línea de comandos y llama a la función de operación correspondiente
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

    // Definir opciones largas para getopt_long
    struct option long_options[] = {
        {"create", no_argument, 0, 'c'},
        {"extract", no_argument, 0, 'x'},
        {"list", no_argument, 0, 't'},
        {"delete", no_argument, 0, 1000}, // Asignar un código único para --delete
        {"update", no_argument, 0, 'u'},
        {"verbose", no_argument, 0, 'v'},
        {"file", required_argument, 0, 'f'},
        {"append", no_argument, 0, 'r'},
        {"pack", no_argument, 0, 'p'},
        {0, 0, 0, 0}};

    int option_index = 0;

    // Analizar opciones de línea de comandos
    while ((opt = getopt_long(argc, argv, "cxtrupvf:", long_options, &option_index)) != -1)
    {
        switch (opt)
        {
        case 'c':
            c_flag = 1; // Crear
            break;
        case 'x':
            x_flag = 1; // Extraer
            break;
        case 't':
            t_flag = 1; // Listar
            break;
        case 'v':
            verbose_level++; // Incrementar nivel de verbosidad
            break;
        case 'f':
            star_filename = optarg; // Nombre del archivo de archivado
            break;
        case 'r':
            r_flag = 1; // Agregar
            break;
        case 'u':
            u_flag = 1; // Actualizar
            break;
        case 'p':
            p_flag = 1; // Empacar (desfragmentar)
            break;
        case 1000: // --delete
            delete_flag = 1;
            break;
        default:
            fprintf(stderr, "Opción desconocida o uso incorrecto\n");
            exit(EXIT_FAILURE);
        }
    }

    // Verificar si se especificó el archivo de archivado
    if (!star_filename)
    {
        fprintf(stderr, "Debe especificar un archivo de salida con -f o --file\n");
        exit(EXIT_FAILURE);
    }

    // Asegurarse de que se especificó exactamente una operación principal
    int operation_count = c_flag + x_flag + t_flag + delete_flag + r_flag + u_flag + p_flag;
    if (operation_count != 1)
    {
        fprintf(stderr, "Debe especificar exactamente una operación principal\n");
        exit(EXIT_FAILURE);
    }

    // Llamar a la función apropiada según la operación especificada
    if (c_flag)
    {
        // Crear nuevo archivador
        if (optind >= argc)
        {
            fprintf(stderr, "Debe especificar al menos un archivo para empaquetar\n");
            exit(EXIT_FAILURE);
        }
        create_star(star_filename, argc - optind, &argv[optind]);
    }
    else if (x_flag)
    {
        // Extraer archivos del archivador
        extract_star(star_filename);
    }
    else if (t_flag)
    {
        // Listar contenido del archivador
        list_star(star_filename);
    }
    else if (r_flag)
    {
        // Agregar archivos al archivador
        if (optind >= argc)
        {
            fprintf(stderr, "Debe especificar al menos un archivo para agregar\n");
            exit(EXIT_FAILURE);
        }
        append_star(star_filename, argc - optind, &argv[optind]);
    }
    else if (u_flag)
    {
        // Actualizar archivos en el archivador
        if (optind >= argc)
        {
            fprintf(stderr, "Debe especificar al menos un archivo para actualizar\n");
            exit(EXIT_FAILURE);
        }
        update_star(star_filename, argc - optind, &argv[optind]);
    }
    else if (p_flag)
    {
        // Empacar (desfragmentar) el archivador
        pack_star(star_filename);
    }
    else if (delete_flag)
    {
        // Eliminar archivos del archivador
        if (optind >= argc)
        {
            fprintf(stderr, "Debe especificar al menos un archivo para eliminar\n");
            exit(EXIT_FAILURE);
        }
        delete_star(star_filename, argc - optind, &argv[optind]);
    }

    return 0;
}

/*
 * Función para crear un nuevo archivador
 * star_filename: Nombre del archivo de archivado a crear
 * file_count: Número de archivos a agregar al archivador
 * files: Array de nombres de archivos a agregar
 */
void create_star(char *star_filename, int file_count, char *files[])
{
    // Verificar que cada archivo existe antes de proceder
    for (int i = 0; i < file_count; i++)
    {
        check_file_exists(files[i]);
    }

    // Abrir el archivo de archivado para escritura (crear o truncar)
    int fd = open(star_filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0)
    {
        perror("Error al crear el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    // Inicializar el encabezado del archivador
    StarHeader header;
    memset(&header, 0, sizeof(StarHeader));
    header.file_count = 0;
    header.free_block_list = -1; // Sin bloques libres inicialmente

    // Escribir el encabezado vacío en el archivo de archivado
    write_header(fd, &header);

    // Agregar cada archivo especificado al archivador
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

    // Escribir el encabezado actualizado de nuevo en el archivador
    write_header(fd, &header);
    close(fd);
}

/*
 * Función para extraer todos los archivos del archivador
 * star_filename: Nombre del archivo de archivado del cual extraer
 */
void extract_star(char *star_filename)
{
    // Abrir el archivo de archivado para lectura
    int fd = open(star_filename, O_RDONLY);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    // Leer el encabezado del archivador
    StarHeader header;
    read_header(fd, &header);

    // Extraer cada archivo en el archivador
    for (int i = 0; i < header.file_count; i++)
    {
        verbose_print("Extrayendo:", 1);
        if (verbose_level >= 1)
        {
            printf(" %s\n", header.files[i].filename);
        }

        // Abrir el archivo de salida para escritura
        int file_fd = open(header.files[i].filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (file_fd < 0)
        {
            perror("Error al crear archivo de salida");
            close(fd);
            exit(EXIT_FAILURE);
        }

        // Leer y escribir bloques de datos
        int current_block = header.files[i].start_block;
        off_t remaining_size = header.files[i].size;

        while (remaining_size > 0 && current_block != -1)
        {
            DataBlock block;
            // Posicionarse en el bloque actual en el archivador
            lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
            // Leer el bloque de datos
            if (read(fd, &block, sizeof(DataBlock)) != sizeof(DataBlock))
            {
                perror("Error al leer bloque de datos");
                close(file_fd);
                close(fd);
                exit(EXIT_FAILURE);
            }

            // Determinar cuántos bytes escribir (puede ser menos que el tamaño del bloque para el último bloque)
            size_t bytes_to_write = remaining_size < sizeof(block.data) ? remaining_size : sizeof(block.data);

            // Escribir datos en el archivo de salida
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

/*
 * Función para listar el contenido del archivador
 * star_filename: Nombre del archivo de archivado a listar
 */
void list_star(char *star_filename)
{
    // Abrir el archivo de archivado para lectura
    int fd = open(star_filename, O_RDONLY);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    // Leer el encabezado del archivador
    StarHeader header;
    read_header(fd, &header);

    printf("Contenido de '%s':\n", star_filename);
    // Listar cada archivo en el archivador
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

/*
 * Función para eliminar archivos del archivador
 * star_filename: Nombre del archivo de archivado
 * file_count: Número de archivos a eliminar
 * files: Array de nombres de archivos a eliminar
 */
void delete_star(char *star_filename, int file_count, char *files[])
{
    // Abrir el archivo de archivado para lectura y escritura
    int fd = open(star_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    // Leer el encabezado del archivador
    StarHeader header;
    read_header(fd, &header);

    // Eliminar cada archivo especificado del archivador
    for (int i = 0; i < file_count; i++)
    {
        int index = find_file_entry(&header, files[i]);
        if (index != -1)
        {
            remove_file_from_star(fd, &header, files[i]);

            // Mensaje consolidado de verbosidad para eliminación
            char message[300];
            snprintf(message, sizeof(message), "Archivo '%s' eliminado del empaquetado.", files[i]);
            verbose_print(message, 1);
        }
        else
        {
            fprintf(stderr, "El archivo '%s' no se encontró en el empaquetado.\n", files[i]);
        }
    }

    // Escribir el encabezado actualizado de nuevo en el archivador
    write_header(fd, &header);
    close(fd);
}

/*
 * Función para agregar archivos al archivador
 * star_filename: Nombre del archivo de archivado
 * file_count: Número de archivos a agregar
 * files: Array de nombres de archivos a agregar
 */
void append_star(char *star_filename, int file_count, char *files[])
{
    // Abrir el archivo de archivado para lectura y escritura
    int fd = open(star_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    // Leer el encabezado del archivador
    StarHeader header;
    read_header(fd, &header);

    // Agregar cada archivo especificado al archivador
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

    // Escribir el encabezado actualizado de nuevo en el archivador
    write_header(fd, &header);
    close(fd);
}

/*
 * Función para actualizar archivos en el archivador
 * star_filename: Nombre del archivo de archivado
 * file_count: Número de archivos a actualizar
 * files: Array de nombres de archivos a actualizar
 */
void update_star(char *star_filename, int file_count, char *files[])
{
    // Abrir el archivo de archivado para lectura y escritura
    int fd = open(star_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    // Leer el encabezado del archivador
    StarHeader header;
    read_header(fd, &header);

    // Actualizar cada archivo especificado en el archivador
    for (int i = 0; i < file_count; i++)
    {
        int index = find_file_entry(&header, files[i]);
        if (index != -1)
        {
            // Eliminar la entrada de archivo antigua
            remove_file_from_star(fd, &header, files[i]);
            // Agregar el nuevo archivo
            add_file_to_star(fd, &header, files[i]);
        }
        else
        {
            fprintf(stderr, "El archivo '%s' no existe en el empaquetado. Use la opción -r para agregarlo.\n", files[i]);
        }
    }

    // Escribir el encabezado actualizado de nuevo en el archivador
    write_header(fd, &header);
    close(fd);
}

/*
 * Función para desfragmentar (empacar) el archivador
 * star_filename: Nombre del archivo de archivado
 */
void pack_star(char *star_filename)
{
    // Abrir el archivo de archivado para lectura y escritura
    int fd = open(star_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado para lectura/escritura");
        exit(EXIT_FAILURE);
    }

    // Leer el encabezado del archivador
    StarHeader header;
    read_header(fd, &header);

    // Recopilar todos los bloques utilizados y crear un mapeo de índices de bloques antiguos a nuevos
    int block_mapping_capacity = 1000;
    int block_mapping_count = 0;
    BlockMapping *block_mappings = malloc(block_mapping_capacity * sizeof(BlockMapping));
    if (!block_mappings)
    {
        perror("Error al asignar memoria");
        close(fd);
        exit(EXIT_FAILURE);
    }

    // Recorrer cada archivo y registrar sus bloques
    for (int i = 0; i < header.file_count; i++)
    {
        int current_block = header.files[i].start_block;
        while (current_block != -1)
        {
            // Expandir el array de mapeo de bloques si es necesario
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

            // Leer el bloque de datos para obtener el índice del siguiente bloque
            lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
            DataBlock block;
            read(fd, &block, sizeof(DataBlock));

            current_block = block.next_block;
        }
    }

    // Ordenar los bloques en orden decreciente de índices old_block
    qsort(block_mappings, block_mapping_count, sizeof(BlockMapping), compare_blocks);

    // Asignar nuevos índices de bloque secuencialmente, comenzando después del encabezado
    int new_block_index = (sizeof(StarHeader) + BLOCK_SIZE - 1) / BLOCK_SIZE; // Calcular número de bloques usados por el encabezado
    for (int i = block_mapping_count - 1; i >= 0; i--)
    {
        block_mappings[i].new_block = new_block_index++;
    }

    // Mover bloques a sus nuevas posiciones y actualizar referencias
    for (int i = 0; i < block_mapping_count; i++)
    {
        int old_block = block_mappings[i].old_block;
        int new_block = block_mappings[i].new_block;

        // Leer el bloque de la posición antigua
        lseek(fd, old_block * BLOCK_SIZE, SEEK_SET);
        DataBlock block;
        read(fd, &block, sizeof(DataBlock));

        // Actualizar next_block del bloque a los nuevos índices
        if (block.next_block != -1)
        {
            // Encontrar el nuevo índice de bloque para block.next_block
            for (int j = 0; j < block_mapping_count; j++)
            {
                if (block_mappings[j].old_block == block.next_block)
                {
                    block.next_block = block_mappings[j].new_block;
                    break;
                }
            }
        }

        // Escribir el bloque en la nueva posición
        lseek(fd, new_block * BLOCK_SIZE, SEEK_SET);
        write(fd, &block, sizeof(DataBlock));
    }

    // Actualizar start_block de cada archivo en el encabezado a los nuevos índices
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

    // Escribir el encabezado actualizado de nuevo en el archivador
    write_header(fd, &header);

    // Truncar el archivo de archivado al nuevo tamaño
    off_t new_size = new_block_index * BLOCK_SIZE;
    ftruncate(fd, new_size); // Truncar el archivo al nuevo tamaño

    close(fd);
    free(block_mappings);

    if (verbose_level > 0)
    {
        printf("Archivo '%s' desfragmentado.\n", star_filename);
    }
}

/*
 * Función para agregar un archivo al archivador
 * fd: Descriptor de archivo del archivador
 * header: Puntero al encabezado del archivador
 * filename: Nombre del archivo a agregar
 */
void add_file_to_star(int fd, StarHeader *header, char *filename)
{
    if (header->file_count >= MAX_FILES)
    {
        fprintf(stderr, "Se alcanzó el número máximo de archivos en el empaquetado.\n");
        exit(EXIT_FAILURE);
    }

    // Copiar el nombre del archivo en la entrada de archivo
    strncpy(header->files[header->file_count].filename, filename, MAX_FILENAME_LENGTH);
    header->files[header->file_count].filename[MAX_FILENAME_LENGTH - 1] = '\0'; // Asegurar terminación nula

    // Abrir el archivo de entrada para lectura
    int file_fd = open(filename, O_RDONLY);
    if (file_fd < 0)
    {
        perror("Error al abrir archivo de entrada");
        exit(EXIT_FAILURE);
    }

    // Obtener el tamaño del archivo de entrada
    struct stat st;
    fstat(file_fd, &st);
    header->files[header->file_count].size = st.st_size;

    int start_block = -1;              // Índice del primer bloque de datos para el archivo
    int prev_block_index = -1;         // Índice del bloque de datos anterior
    off_t remaining_size = st.st_size; // Bytes restantes por leer del archivo de entrada

    // Leer el archivo de entrada y escribir bloques de datos en el archivador
    while (remaining_size > 0)
    {
        int current_block;
        // Verificar si hay bloques libres para reutilizar
        if (header->free_block_list != -1)
        {
            // Reutilizar un bloque libre
            current_block = header->free_block_list;
            DataBlock free_block;
            lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
            read(fd, &free_block, sizeof(DataBlock));
            header->free_block_list = free_block.next_block; // Actualizar lista de bloques libres
        }
        else
        {
            // No hay bloques libres; agregar al final del archivador
            current_block = lseek(fd, 0, SEEK_END) / BLOCK_SIZE;
        }

        if (start_block == -1)
        {
            start_block = current_block; // Establecer bloque inicial para el archivo
        }

        // Inicializar un nuevo bloque de datos
        DataBlock block;
        memset(&block, 0, sizeof(DataBlock));
        ssize_t bytes_read = read(file_fd, block.data, sizeof(block.data));
        if (bytes_read < 0)
        {
            perror("Error al leer el archivo");
            close(file_fd);
            exit(EXIT_FAILURE);
        }

        block.next_block = -1; // Inicializar next_block como -1

        // Escribir el bloque de datos en el archivador
        lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
        write(fd, &block, sizeof(DataBlock));

        // Actualizar next_block del bloque anterior para apuntar al bloque actual
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

    // Actualizar la entrada de archivo con el bloque inicial
    header->files[header->file_count].start_block = start_block;
    header->file_count++;

    close(file_fd);

    // Mensaje consolidado de verbosidad
    char message[300];
    snprintf(message, sizeof(message), "Archivo '%s' agregado al empaquetado.", filename);
    verbose_print(message, 1);
}

/*
 * Función para eliminar un archivo del archivador
 * fd: Descriptor de archivo del archivador
 * header: Puntero al encabezado del archivador
 * filename: Nombre del archivo a eliminar
 */
void remove_file_from_star(int fd, StarHeader *header, char *filename)
{
    int index = find_file_entry(header, filename);
    if (index == -1)
    {
        fprintf(stderr, "El archivo '%s' no se encontró en el empaquetado.\n", filename);
        return;
    }

    // Agregar los bloques del archivo a la lista de bloques libres
    int current_block = header->files[index].start_block;
    while (current_block != -1)
    {
        DataBlock block;
        lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
        read(fd, &block, sizeof(DataBlock));

        int next_block = block.next_block;

        // Agregar el bloque a la lista de bloques libres
        block.next_block = header->free_block_list;
        header->free_block_list = current_block;

        // Escribir el bloque actualizado de nuevo en el archivador
        lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
        write(fd, &block, sizeof(DataBlock));

        current_block = next_block;
    }

    // Eliminar la entrada de archivo del encabezado
    for (int j = index; j < header->file_count - 1; j++)
    {
        header->files[j] = header->files[j + 1];
    }
    header->file_count--;
}

/*
 * Función para imprimir un mensaje basado en el nivel de verbosidad
 * message: El mensaje a imprimir
 * level: El nivel de verbosidad requerido para imprimir el mensaje
 */
void verbose_print(const char *message, int level)
{
    if (verbose_level >= level)
    {
        printf("%s\n", message);
    }
}

/*
 * Función para encontrar el índice de una entrada de archivo en el encabezado del archivador
 * header: Puntero al encabezado del archivador
 * filename: Nombre del archivo a encontrar
 * Retorna: Índice del archivo en el encabezado, o -1 si no se encuentra
 */
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

/*
 * Función para leer el encabezado del archivador desde el archivo
 * fd: Descriptor de archivo del archivador
 * header: Puntero a la estructura de encabezado a llenar
 */
void read_header(int fd, StarHeader *header)
{
    lseek(fd, 0, SEEK_SET); // Posicionarse al inicio del archivo
    read(fd, header, sizeof(StarHeader));
}

/*
 * Función para escribir el encabezado del archivador en el archivo
 * fd: Descriptor de archivo del archivador
 * header: Puntero a la estructura de encabezado a escribir
 */
void write_header(int fd, StarHeader *header)
{
    lseek(fd, 0, SEEK_SET); // Posicionarse al inicio del archivo
    write(fd, header, sizeof(StarHeader));
}