#include <linux/fs.h>
#include <linux/uaccess.h>
#include "osfs.h"

/**
 * Function: osfs_read
 * Description: Reads data from a file.
 * Inputs:
 *   - filp: The file pointer representing the file to read from.
 *   - buf: The user-space buffer to copy the data into.
 *   - len: The number of bytes to read.
 *   - ppos: The file position pointer.
 * Returns:
 *   - The number of bytes read on success.
 *   - 0 if the end of the file is reached.
 *   - -EFAULT if copying data to user space fails.
 */
static ssize_t osfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_read = 0;

    // If the file has not been allocated a data block, it indicates the file is empty
    if (osfs_inode->i_blocks == 0)
        return 0;

    // if offset out of file size, return 0
    if (*ppos >= osfs_inode->i_size)
        return 0;

    // if the read length exceeds the file size, adjust the length
    if (*ppos + len > osfs_inode->i_size)
        len = osfs_inode->i_size - *ppos;
    
    ssize_t bytes_to_read = len;
    
    pr_info("osfs_read: Reading %ld bytes from %lld\n", len, *ppos);

    // traverse to the block containing ppos
    uint32_t fat_block_pointer = osfs_inode->i_block;
    uint32_t current_block_index = 0;
    while(*ppos > (current_block_index + 1) * BLOCK_SIZE)
    {
        fat_block_pointer = sb_info->fat[fat_block_pointer];
        current_block_index++;
    }

    // read data
    while(bytes_to_read > 0)
    {
        ssize_t current_block_bytes_to_read = (*ppos % BLOCK_SIZE + bytes_to_read) > BLOCK_SIZE ? (BLOCK_SIZE - *ppos % BLOCK_SIZE) : bytes_to_read;
        pr_info("osfs_read: Reading %ld bytes from block %u\n", current_block_bytes_to_read, fat_block_pointer);
        data_block = sb_info->data_blocks + fat_block_pointer * BLOCK_SIZE + *ppos % BLOCK_SIZE;
        if(copy_to_user(buf + bytes_read, data_block, current_block_bytes_to_read))
        {
            return -EFAULT;
        }
        bytes_read += current_block_bytes_to_read;
        bytes_to_read -= current_block_bytes_to_read;
        fat_block_pointer = sb_info->fat[fat_block_pointer];
        current_block_index++;
    }

    *ppos += bytes_read;
    pr_info("osfs_read: %ld bytes read\n", bytes_read);

    return bytes_read;
}


/**
 * Function: osfs_write
 * Description: Writes data to a file.
 * Inputs:
 *   - filp: The file pointer representing the file to write to.
 *   - buf: The user-space buffer containing the data to write.
 *   - len: The number of bytes to write.
 *   - ppos: The file position pointer.
 * Returns:
 *   - The number of bytes written on success.
 *   - -EFAULT if copying data from user space fails.
 *   - Adjusted length if the write exceeds the block size.
 */
static ssize_t osfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{   
    //Step1: Retrieve the inode and filesystem information
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_written = 0;
    ssize_t bytes_to_write = len;
    int ret;

    pr_info("osfs_write: Writing %ld bytes from %lld\n", len, *ppos);
    // Step2: Check if a data block has been allocated; if not, allocate one
    if(osfs_inode->i_blocks == 0)
    {
        pr_info("osfs_write: No data block allocated\n");
        ret = osfs_alloc_data_block(sb_info, &osfs_inode->i_block);
        if(ret)
        {
            pr_err("osfs_write: Failed to allocate data block\n");
            return ret;
        }
        osfs_inode->i_size = 0;
        inode->i_size = 0;
        // allocated 1 block
        osfs_inode->i_blocks = 1;
        inode->i_blocks = 1;
    }

    // Allocate blank blocks between current end to ppos
    uint32_t new_block;
    uint32_t fat_block_pointer = osfs_inode->i_block;   // always points to valid block
    uint32_t current_block_index = 0;

    while(bytes_to_write > 0)
    {
        // TODO: if current is invalid, allocate new block
        if(current_block_index >= osfs_inode->i_blocks)
        {
            ret = osfs_alloc_data_block(sb_info, &new_block);
            if(ret)
            {
                pr_err("osfs_write: Failed to allocate data block\n");
                return ret;
            }
            pr_info("osfs_write: Allocated new block %u\n", new_block);
            // update fat
            sb_info->fat[fat_block_pointer] = new_block;
            fat_block_pointer = new_block;
            osfs_inode->i_blocks++;
            inode->i_blocks++;
        }
        // TODO: if ppos is not in current block, move to next block
        if(*ppos > (current_block_index + 1) * BLOCK_SIZE)
        {
            // if next block is invalid, dont change fat ptr
            if(current_block_index + 1 < osfs_inode->i_blocks)
            {
                fat_block_pointer = sb_info->fat[fat_block_pointer];
            }
        }
        // TODO: else write data to current block
        else
        {
            ssize_t current_block_bytes_to_write = (*ppos % BLOCK_SIZE + bytes_to_write) > BLOCK_SIZE ? (BLOCK_SIZE - *ppos % BLOCK_SIZE) : bytes_to_write;
            data_block = sb_info->data_blocks + fat_block_pointer * BLOCK_SIZE + *ppos % BLOCK_SIZE;
            if(copy_from_user(data_block, buf + bytes_written, current_block_bytes_to_write))
            {
                return -EFAULT;
            }
            bytes_written += current_block_bytes_to_write;
            bytes_to_write -= current_block_bytes_to_write;
        }
        current_block_index++;
    }

    // Update inode & osfs_inode attribute
    // extend size if needed
    osfs_inode->i_size = (*ppos + bytes_written) > osfs_inode->i_size ? (*ppos + bytes_written) : osfs_inode->i_size;
    inode->i_size = osfs_inode->i_size;
    *ppos += len;

    // Step6: Return the number of bytes written

    pr_info("osfs_write: %ld bytes written, new size: %u\n", bytes_written, osfs_inode->i_size);
    return bytes_written;
}

/**
 * Struct: osfs_file_operations
 * Description: Defines the file operations for regular files in osfs.
 */
const struct file_operations osfs_file_operations = {
    .open = generic_file_open, // Use generic open or implement osfs_open if needed
    .read = osfs_read,
    .write = osfs_write,
    .llseek = default_llseek,
    // Add other operations as needed
};

/**
 * Struct: osfs_file_inode_operations
 * Description: Defines the inode operations for regular files in osfs.
 * Note: Add additional operations such as getattr as needed.
 */
const struct inode_operations osfs_file_inode_operations = {
    // Add inode operations here, e.g., .getattr = osfs_getattr,
};
