#include <linux/module.h>       /* Needed by all modules */
#include <linux/kernel.h>       /* Needed for KERN_INFO  */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/fs.h>           /* libfs stuff           */
#include <linux/buffer_head.h>  /* buffer_head           */
#include <linux/slab.h>         /* kmem_cache            */
#include "assoofs.h"

/*
 *  Operaciones sobre ficheros
 */
ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos);
ssize_t assoofs_write(struct file * filp, const char __user * buf, size_t len, loff_t * ppos);
const struct file_operations assoofs_file_operations = {
    .read = assoofs_read,
    .write = assoofs_write,
};

ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos) {
    printk(KERN_INFO "Read request\n");
    return -1;
}

ssize_t assoofs_write(struct file * filp, const char __user * buf, size_t len, loff_t * ppos) {
    printk(KERN_INFO "Write request\n");
    return -1;
}

/*
 *  Operaciones sobre directorios
 */
static int assoofs_iterate(struct file *filp, struct dir_context *ctx);
const struct file_operations assoofs_dir_operations = {
    .owner = THIS_MODULE,
    .iterate = assoofs_iterate,
};


// funciones auxiliares

void assoofs_save_sb_info(struct super_block *vsb){
	
	// leer de disco la informacion persistente del superbloque con sb_bread y sobreescribir el campo b_data con la informacion en memoria:
	struct buffer_head *bh;
	struct assoofs_super_block *sb = vsb->s_fs_info; // Informacion persistente del superbloque en memoria
	bh = sb_bread(vsb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
	bh->b_data = (char *)sb; // Sobreescribo los datos de disco con la informacion en memoria

	// marcar el buffer como sucio y sincronizar para que el cambio pase a disco
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

int assoofs_sb_get_a_freeblock(struct super_block *sb, uint64_t *block){
	//obtener la informacion persistente del superbloque.
	struct assoofs_super_block_info *assoofs_sb = sb->s_fs_info;
	
	int i;
	for (i = 2; i < ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++)
		if (assoofs_sb->free_blocks & (1 << i))
			break; // cuando aparece el primer bit 1 en free_block dejamos de recorrer el mapa de bits, i tiene la posicion del primer bloque libre

	*block = i; // Escribimos el valor de i en la direccion de memoria indicada como segundo argumento en la funcion

	//Actualizar el valor de free_blocks y guardar los cambios en el superbloque
	assoofs_sb->free_blocks &= ~(1 << i);
	assoofs_save_sb_info(sb);
	return 0;
}

// guardar en disco la informacion persistente de un nuevo inodo
void assoofs_add_inode_info(struct super_block *sb, struct assoofs_inode_info *inode){

	// acceder a la informacion persistente en el superbloque para obtener el contador de inodos
	uint64_t count;
	count = ((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count; // obtengo el numero de inodos de la informacion persistente del superbloque

	// leer de disco el bloque que contiene el almacen de inodos
	struct buffer_head *bh;
	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);

	// obtener un puntero al final del almacen y escribir un nuevo valor al final
	struct assoofs_super_block_info *assoofs_sb;
	inode = (struct assoofs_inode_info *)bh->b_data;
	inode += assoofs_sb->inodes_count;
	memcpy(inode, inode, sizeof(struct assoofs_inode_info));

	// marcar el bloque como sucio y sincronizar
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	// actualizar el contador de inodos de la informacion persistente del superbloque y guardar los cambios
	assoofs_sb->inodes_count++;
	assoofs_save_sb_info(sb);
}

// actualizar en disco la informacion persistente de un inodo
int assoofs_save_inode_info(struct super_block *sb, struct assoofs_inode_info *inode_info){

	// obtener de disco el almacen de inodos
	struct buffer_head *bh;
	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);

	// buscar los datos de inode_info en el almacen
	struct assoofs_inode_info *inode_pos;
	inode_pos = assoofs_search_inode_info(sb, (struct assoofs_inode_info *)bh->b_data, inode_info);

	// actualizar el inodo
	memcpy(inode_pos, inode_info, sizeof(*inode_pos));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	return 0;
}

// obtener un puntero a la informacion persistente de un inodo concreto
struct assoofs_inode_info *assoofs_search_inode_info(struct super_block *sb, struct assoofs_inode_info *start, struct
assoofs_inode_info *search){

	// recorrer el almacen de inodos hasta encontrar los datos del inodo
	uint64_t count = 0;
	while (start->inode_no != search->inode_no && count < ((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count) {
		count++;
		start++;
	}

	if (start->inode_no == search->inode_no)
		return start;
	else
		return NULL;
}

/*
 *  Operaciones sobre inodos
 */
static int assoofs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags);
static int assoofs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);
static struct inode_operations assoofs_inode_ops = {
    .create = assoofs_create,
    .lookup = assoofs_lookup,
    .mkdir = assoofs_mkdir,
};

struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no){
	struct assoofs_inode_info *inode_info = NULL;
	struct buffer_head *bh;
	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
	inode_info = (struct assoofs_inode_info *)bh->b_data;

	struct assoofs_super_block_info *afs_sb = sb->s_fs_info;
	struct assoofs_inode_info *buffer = NULL;
	int i;
	for (i = 0; i < afs_sb->inodes_count; i++) {
		if (inode_info->inode_no == inode_no) {
		buffer = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
		memcpy(buffer, inode_info, sizeof(*buffer));
		break;
		}
		inode_info++;
	}

	brelse(bh);
	return buffer;
}

static struct inode *assoofs_get_inode(struct super_block *sb, int ino){
	struct inode *inode;
	struct assoofs_inode_info *inode_info;

	inode_info = assoofs_get_inode_info(sb, ino);

	inode = new_inode(sb);
	inode->i_ino = ino;
	inode->i_sb = sb;
	inode->i_op = &assoofs_inode_ops;

	if (S_ISDIR(inode_info->mode))
		inode->i_fop = &assoofs_dir_operations;
	else if (S_ISREG(inode_info->mode))
		inode->i_fop = &assoofs_file_operations;
	else
		printk(KERN_ERR "Unknown inode type. Neither a directory nor a file.");

	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_private = inode_info;

	return inode;
}



static int assoofs_iterate(struct file *filp, struct dir_context *ctx) {
    printk(KERN_INFO "Iterate request\n");
    return -1;
}

struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags) {
    printk(KERN_INFO "Lookup request\n");
    
    // acceder al bloque de disco con el contenido del directorio apuntado por parent_inode
    struct assoofs_inode_info *parent_info = parent_inode->i_private;
	struct super_block *sb = parent_inode->i_sb;
	struct buffer_head *bh;
	int i;
	bh = sb_bread(sb, parent_info->data_block_number);

	// recorrer el contenido del directorio buscando la entrada cuyo nombre 
	// se corresponda con el que buscamos. Si se localiza
	// la entrada, entonces tenemos construir el inodo correspondiente.
	struct assoofs_dir_record_entry *record;
	record = (struct assoofs_dir_record_entry *)bh->b_data;
	for (i=0; i < parent_info->dir_children_count; i++) {
		if (!strcmp(record->filename, child_dentry->d_name.name)) {
			struct inode *inode = assoofs_get_inode(sb, record->inode_no); // Funcion auxiliar que obtine la informacion de un inodo a partir de su numero de inodo.
			inode_init_owner(inode, parent_inode, ((struct assoofs_inode_info *)inode->i_private)->mode);
			d_add(child_dentry, inode);
			return NULL;
		}
		record++;
	}
    return NULL;
}


static int assoofs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
    printk(KERN_INFO "New file request\n");
    
    //1. Crear el nuevo inodo
    struct inode *inode;
	uint64_t count;
	struct super_block *sb;
	sb = dir->i_sb; // obtengo un puntero al superbloque desde dir
	count = ((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count; // obtengo el numero de inodos de la informacion persistente del superbloque
	inode = new_inode(sb);
	if(count > ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED){
		return -1;
	}
	inode->i_ino = count + 1; // Asigno numero al nuevo inodo a partir de count

	struct assoofs_inode_info *inode_info;
	inode_info = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
	inode_info->inode_no = inode->i_ino;
	inode_info->mode = mode; // mode me llega como argumento
	inode_info->file_size = 0;
	inode->i_private = inode_info;
	inode_init_owner(inode, dir, mode);
	d_add(dentry, inode);
	inode->i_fop=&assoofs_file_operations;

	assoofs_sb_get_a_freeblock(sb, &inode_info->data_block_number);
	assoofs_add_inode_info(sb, inode_info);

	//2. Modificar el contenido del directorio padre
	struct assoofs_inode_info *parent_inode_info;
	struct assoofs_dir_record_entry *dir_contents;
	struct buffer_head *bh;

	parent_inode_info = dir->i_private;
	bh = sb_bread(sb, parent_inode_info->data_block_number);
	
	dir_contents = (struct assoofs_dir_record_entry *)bh->b_data;
	dir_contents += parent_inode_info->dir_children_count;
	dir_contents->inode_no = inode_info->inode_no; // inode_info es la informaci ́on persistente del inodo creado en el paso 2.
	
	strcpy(dir_contents->filename, dentry->d_name.name);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	// 3. Actualizar la informacion persistente del inodo padre indicando que ahora tiene un archivo mas
	parent_inode_info->dir_children_count++;
	assoofs_save_inode_info(sb, parent_inode_info);
   
    return -1;
}

static int assoofs_mkdir(struct inode *dir , struct dentry *dentry, umode_t mode) {
    printk(KERN_INFO "New directory request\n");
    return -1;
}

/*
 *  Operaciones sobre el superbloque
 */
static const struct super_operations assoofs_sops = {
    .drop_inode = generic_delete_inode,
};

/*
 *  Inicialización del superbloque
 */
int assoofs_fill_super(struct super_block *sb, void *data, int silent) {   
    printk(KERN_INFO "assoofs_fill_super request\n");
    // 1.- Leer la información persistente del superbloque del dispositivo de bloques
    
    struct buffer_head *bh;
	struct assoofs_super_block_info *assoofs_sb;
	
	bh = sb_bread(sb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER); // sb lo recibe assoofs_fill_super como argumento
	assoofs_sb = (struct assoofs_super_block_info *)bh->b_data;
    
    // 2.- Comprobar los parámetros del superbloque

    if(assoofs_sb->magic != 0x20200406){
    	return -1;
    }
    if(assoofs_sb->block_size != 4096){
    	return -1;
    }

    // 3.- Escribir la información persistente leída del dispositivo de bloques en el superbloque sb, incluído el campo s_op con las operaciones que soporta.
    
    sb->s_magic = assoofs_sb->magic;
    sb->s_maxbytes = assoofs_sb->block_size;
    sb->s_op = &assoofs_sops;
    sb->s_fs_info = assoofs_sb;

    // 4.- Crear el inodo raíz y asignarle operaciones sobre inodos (i_op) y sobre directorios (i_fop)

    struct inode *root_inode;
    root_inode = new_inode(sb);
    inode_init_owner(root_inode, NULL, S_IFDIR); // S_IFDIR para directorios, S_IFREG para ficheros.

    root_inode->i_ino = ASSOOFS_ROOTDIR_INODE_NUMBER; // numero de inodo
	root_inode->i_sb = sb; // puntero al superbloque
	root_inode->i_op = &assoofs_inode_ops; // direccion de una variable de tipo struct inode_operations previamente declarada
	root_inode->i_fop = &assoofs_dir_operations; // direccion de una variable de tipo struct file_operations previamente declarada.
	root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode); // fechas.
	root_inode->i_private = assoofs_get_inode_info(sb, ASSOOFS_ROOTDIR_INODE_NUMBER); // Informacion persistente del inodo

	sb->s_root = d_make_root(root_inode);
	if(!sb->s_root){
		brelse(bh);
		return -1;
	}

	brelse(bh); // liberar memoria despues de utilizar el bloque
    return 0;
}

/*
 *  Montaje de dispositivos assoofs
 */
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
    printk(KERN_INFO "assoofs_mount request\n");
    struct dentry *ret = mount_bdev(fs_type, flags, dev_name, data, assoofs_fill_super);
    // Control de errores a partir del valor de ret. En este caso se puede utilizar la macro IS_ERR: if (IS_ERR(ret)) ...
}

/*
 *  assoofs file system type
 */
static struct file_system_type assoofs_type = {
    .owner   = THIS_MODULE,
    .name    = "assoofs",
    .mount   = assoofs_mount,
    .kill_sb = kill_litter_super,
};

// registrar el nuevo sistema de ficheros en el kernel.
static int __init assoofs_init(void) {
    printk(KERN_INFO "assoofs_init request\n");
    int ret = register_filesystem(&assoofs_type);
    // Control de errores a partir del valor de ret
}

// eliminar la informaci ́on del nuevo sistema de ficheros del kernel.
static void __exit assoofs_exit(void) {
    printk(KERN_INFO "assoofs_exit request\n");
    int ret = unregister_filesystem(&assoofs_type);
    // Control de errores a partir del valor de ret
}

module_init(assoofs_init);
module_exit(assoofs_exit);

