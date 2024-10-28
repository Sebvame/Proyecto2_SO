#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>

#define BLOCK_SIZE 262144       // Tamaño del bloque: 256K
#define MAX_FILES 250           // Máximo número de archivos en el archivo
#define MAX_FILENAME_LENGTH 256 // Longitud máxima para nombres de archivo

// Estructura para análisis de fragmentación
typedef struct
{
    int total_blocks;          // Total de bloques en el archivo
    int used_blocks;           // Bloques en uso
    int free_blocks;           // Bloques libres
    int fragmented_blocks;     // Bloques que causan fragmentación
    float fragmentation_ratio; // Ratio de fragmentación
    int largest_free_chunk;    // Mayor número de bloques libres contiguos
    int smallest_free_chunk;   // Menor número de bloques libres contiguos
    int *block_status;         // Array para estado de bloques (1=usado, 0=libre)
} FragmentationInfo;

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
FragmentationInfo analyze_fragmentation(int fd, StarHeader *header);
void print_fragmentation_visualization(FragmentationInfo *info, StarHeader *header);

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
            printf("Archivo '%s' eliminado del empaquetado.\n", files[i]);
        }
        else
        {
            fprintf(stderr, "El archivo '%s' no se encontró en el empaquetado.\n", files[i]);
        }
    }

    // Write header before analysis
    write_header(fd, &header);

    // Analyze fragmentation after deletion
    if (verbose_level >= 1)
    {
        FragmentationInfo frag_info = analyze_fragmentation(fd, &header);
        if (frag_info.block_status != NULL)
        {
            print_fragmentation_visualization(&frag_info, &header);
            free(frag_info.block_status);
        }
    }
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

FragmentationInfo analyze_fragmentation(int fd, StarHeader *header)
{
    FragmentationInfo info;
    memset(&info, 0, sizeof(FragmentationInfo));

    // Get file size and calculate total blocks
    off_t file_size = lseek(fd, 0, SEEK_END);
    info.total_blocks = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // Allocate block status array
    info.block_status = calloc(info.total_blocks, sizeof(int));
    if (!info.block_status)
    {
        fprintf(stderr, "Error: No se pudo asignar memoria\n");
        return info;
    }

    // Mark header blocks
    int header_blocks = (sizeof(StarHeader) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (int i = 0; i < header_blocks && i < info.total_blocks; i++)
    {
        info.block_status[i] = 1;
        info.used_blocks++;
    }

    // Mark blocks used by files
    for (int i = 0; i < header->file_count; i++)
    {
        int current_block = header->files[i].start_block;
        while (current_block != -1 && current_block < info.total_blocks)
        {
            if (!info.block_status[current_block])
            { // Only count if not already marked
                info.block_status[current_block] = 1;
                info.used_blocks++;
            }

            // Read next block
            DataBlock block;
            lseek(fd, current_block * BLOCK_SIZE, SEEK_SET);
            if (read(fd, &block, sizeof(DataBlock)) != sizeof(DataBlock))
            {
                break;
            }
            current_block = block.next_block;
        }
    }

    // Calculate free blocks
    info.free_blocks = info.total_blocks - info.used_blocks;

    // Analyze fragmentation
    int current_free_chunk = 0;
    info.fragmented_blocks = 0;
    info.largest_free_chunk = 0;
    info.smallest_free_chunk = info.total_blocks;

    for (int i = 0; i < info.total_blocks; i++)
    {
        if (info.block_status[i] == 0)
        {
            current_free_chunk++;
            if (i == info.total_blocks - 1 || info.block_status[i + 1] == 1)
            {
                if (current_free_chunk > 0)
                {
                    // Update largest/smallest chunks
                    if (current_free_chunk > info.largest_free_chunk)
                    {
                        info.largest_free_chunk = current_free_chunk;
                    }
                    if (current_free_chunk < info.smallest_free_chunk)
                    {
                        info.smallest_free_chunk = current_free_chunk;
                    }
                    // Count single blocks as fragmented
                    if (current_free_chunk == 1)
                    {
                        info.fragmented_blocks++;
                    }
                }
                current_free_chunk = 0;
            }
        }
    }

    // If no free blocks were found, reset smallest_free_chunk
    if (info.free_blocks == 0)
    {
        info.smallest_free_chunk = 0;
    }

    // Calculate fragmentation ratio
    info.fragmentation_ratio = info.free_blocks > 0 ? (float)info.fragmented_blocks / info.free_blocks : 0;

    return info;
}

void print_fragmentation_visualization(FragmentationInfo *info, StarHeader *header)
{
    if (!info || !info->block_status)
    {
        fprintf(stderr, "Error: Información de fragmentación no válida\n");
        return;
    }

    printf("\nEstado de Fragmentación:\n");
    printf("------------------------\n");

    // Basic block info - now with validation
    printf("Bloques totales: %d\n", info->total_blocks);
    printf("Bloques usados:  %d (%.1f%%)\n",
           info->used_blocks,
           (float)info->used_blocks / info->total_blocks * 100);
    printf("Bloques libres: %d\n", info->free_blocks);

    // Block distribution
    printf("\nDistribución de bloques:\n");
    printf("H = Header | █ = Usado | ░ = Libre\n");

    printf("Bloque:  ");
    for (int i = 0; i < info->total_blocks; i++)
    {
        printf("%-2d ", i);
    }
    printf("\nEstado:  ");

    int header_blocks = (sizeof(StarHeader) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (int i = 0; i < info->total_blocks; i++)
    {
        if (i < header_blocks)
        {
            printf("H  ");
        }
        else if (info->block_status[i])
        {
            printf("█  ");
        }
        else
        {
            printf("░  ");
        }
    }

    printf("\n\nContenido:\n");
    for (int i = 0; i < header->file_count; i++)
    {
        printf("- Bloque %d: %s (%lld bytes)\n",
               header->files[i].start_block,
               header->files[i].filename,
               (long long)header->files[i].size);
    }

    if (info->fragmented_blocks > 0)
    {
        printf("\nFragmentación: %.1f%% (%d bloques)\n",
               info->fragmentation_ratio * 100,
               info->fragmented_blocks);
    }
}

/*
 * Función para desfragmentar (empacar) el archivador
 * star_filename: Nombre del archivo de archivado
 */
void pack_star(char *star_filename)
{
    int fd = open(star_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Error al abrir el archivo empaquetado");
        exit(EXIT_FAILURE);
    }

    StarHeader orig_header;
    read_header(fd, &orig_header);

    // Análisis inicial
    FragmentationInfo before_info = analyze_fragmentation(fd, &orig_header);
    if (verbose_level >= 1)
    {
        printf("\nAntes de desfragmentar:");
        print_fragmentation_visualization(&before_info, &orig_header);
    }

    // Si no hay archivos, salir
    if (orig_header.file_count == 0)
    {
        free(before_info.block_status);
        close(fd);
        return;
    }

    // Crear nueva estructura header
    StarHeader new_header;
    memcpy(&new_header, &orig_header, sizeof(StarHeader));
    new_header.free_block_list = -1;

    // Calcular bloques del header
    int header_blocks = (sizeof(StarHeader) + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // Crear buffer temporal para almacenar todos los bloques de datos
    DataBlock *data_blocks = malloc(orig_header.file_count * sizeof(DataBlock));
    if (!data_blocks)
    {
        perror("Error de memoria");
        free(before_info.block_status);
        close(fd);
        exit(EXIT_FAILURE);
    }

    // Primero, recopilar todos los bloques de datos en orden
    int data_block_count = 0;
    for (int i = 0; i < orig_header.file_count; i++)
    {
        // Leer el bloque de datos del archivo
        lseek(fd, orig_header.files[i].start_block * BLOCK_SIZE, SEEK_SET);
        if (read(fd, &data_blocks[data_block_count], sizeof(DataBlock)) == sizeof(DataBlock))
        {
            // Actualizar el header para el nuevo inicio de archivo
            new_header.files[i].start_block = header_blocks + data_block_count;
            data_block_count++;
        }
    }

    // Segundo, escribir el nuevo header
    lseek(fd, 0, SEEK_SET);
    write_header(fd, &new_header);

    // Tercero, escribir los bloques de datos de manera contigua
    for (int i = 0; i < data_block_count; i++)
    {
        // Actualizar el puntero al siguiente bloque
        data_blocks[i].next_block = (i < data_block_count - 1) ? (header_blocks + i + 1) : -1;

        // Escribir el bloque en su nueva posición
        lseek(fd, (header_blocks + i) * BLOCK_SIZE, SEEK_SET);
        if (write(fd, &data_blocks[i], sizeof(DataBlock)) != sizeof(DataBlock))
        {
            perror("Error al escribir bloque");
            free(data_blocks);
            free(before_info.block_status);
            close(fd);
            exit(EXIT_FAILURE);
        }
    }

    // Truncar el archivo al nuevo tamaño
    int total_blocks = header_blocks + data_block_count;
    off_t new_size = total_blocks * BLOCK_SIZE;
    if (ftruncate(fd, new_size) != 0)
    {
        perror("Error al truncar archivo");
    }

    // Análisis final
    FragmentationInfo after_info = analyze_fragmentation(fd, &new_header);
    if (verbose_level >= 1)
    {
        printf("\nDespués de desfragmentar:");
        print_fragmentation_visualization(&after_info, &new_header);

        int blocks_saved = before_info.total_blocks - after_info.total_blocks;
        printf("\nResumen de optimización:\n");
        printf("- Tamaño antes: %d bloques (%ld bytes)\n",
               before_info.total_blocks, (long)before_info.total_blocks * BLOCK_SIZE);
        printf("- Tamaño después: %d bloques (%ld bytes)\n",
               after_info.total_blocks, (long)after_info.total_blocks * BLOCK_SIZE);
        if (blocks_saved > 0)
        {
            printf("- Espacio recuperado: %d bloques (%ld bytes)\n",
                   blocks_saved, (long)blocks_saved * BLOCK_SIZE);
        }
    }

    // Limpieza
    free(data_blocks);
    free(before_info.block_status);
    free(after_info.block_status);
    close(fd);
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