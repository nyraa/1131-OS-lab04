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
    ssize_t bytes_read;

    // If the file has not been allocated a data block, it indicates the file is empty
    if (osfs_inode->i_blocks == 0)
        return 0;

    // if offset out of file size, return 0
    if (*ppos >= osfs_inode->i_size)
        return 0;

    // if the read length exceeds the file size, adjust the length
    if (*ppos + len > osfs_inode->i_size)
        len = osfs_inode->i_size - *ppos;
    
    pr_info("osfs_read: Reading %ld bytes from %lld\n", len, *ppos);

    // (data_blocks start address) + (block index * block size) + (seek_offset)
    data_block = sb_info->data_blocks + osfs_inode->i_block * BLOCK_SIZE + *ppos;
    // copy len bytes from data_block to user space
    if (copy_to_user(buf, data_block, len))
        return -EFAULT;

    *ppos += len;
    bytes_read = len;

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

    // Step3:
    // Allocate blank blocks between current end to ppos
    uint32_t new_block;
    uint32_t fat_block_pointer = osfs_inode->i_block;
    uint32_t pos;
    uint32_t block_count;

    // go to last valid block or block ppos points to
    pr_info("osfs_write: Finding last valid block or block ppos points to\n");
    for(pos = 0, block_count = 0; pos + BLOCK_SIZE < *ppos && block_count < osfs_inode->i_blocks; pos += BLOCK_SIZE, ++block_count)
    {
        fat_block_pointer = sb_info->fat[fat_block_pointer];
        pr_info("osfs_write: pos = %d, block_count = %d, fat_ptr = %d\n", pos, block_count, fat_block_pointer);
    }
    // allocate new empty blocks until reach ppos position
    // while ppos not in position of last block
    pr_info("osfs_write: Allocating new empty blocks until reach ppos position\n");
    while(pos + BLOCK_SIZE < *ppos)
    {
        // append new block
        pr_info("osfs_write: Allocating empty block\n");
        ret = osfs_alloc_data_block(sb_info, &new_block);
        if(ret)
        {
            pr_err("osfs_write: Failed to allocate data block\n");
            return ret;
        }
        memset(sb_info->data_blocks + new_block * BLOCK_SIZE, 0, BLOCK_SIZE);
        osfs_inode->i_blocks++;
        inode->i_blocks = osfs_inode->i_blocks;
        sb_info->fat[fat_block_pointer] = new_block;
        fat_block_pointer = new_block;
        pos += BLOCK_SIZE;
    }
    // write data to the last block
    ssize_t current_block_bytes_to_write = (*ppos % BLOCK_SIZE + len > BLOCK_SIZE) ? (BLOCK_SIZE - *ppos % BLOCK_SIZE) : len;
    pr_info("osfs_write: Writing %ld bytes to the last block %d\n", current_block_bytes_to_write, fat_block_pointer);
    data_block = sb_info->data_blocks + fat_block_pointer * BLOCK_SIZE + (*ppos % BLOCK_SIZE);
    if(copy_from_user(data_block, buf, current_block_bytes_to_write))
        return -EFAULT;
    bytes_to_write -= current_block_bytes_to_write;
    bytes_written += current_block_bytes_to_write;

    // allocate new blocks and write data to them
    pr_info("osfs_write: Allocating new blocks and writing data to them\n");
    while(*ppos + len > osfs_inode->i_blocks * BLOCK_SIZE)
    {
        pr_info("osfs_write: Allocating new block, %ld bytes left\n", bytes_to_write);
        ret = osfs_alloc_data_block(sb_info, &new_block);
        if(ret)
        {
            pr_err("osfs_write: Failed to allocate data block\n");
            return ret;
        }
        osfs_inode->i_blocks++;
        inode->i_blocks = osfs_inode->i_blocks;
        sb_info->fat[fat_block_pointer] = new_block;
        fat_block_pointer = new_block;
        data_block = sb_info->data_blocks + new_block * BLOCK_SIZE;
        // write min(BLOCK_SIZE, bytes_to_write) bytes to the new block
        current_block_bytes_to_write = bytes_to_write < BLOCK_SIZE ? bytes_to_write : BLOCK_SIZE;
        if(copy_from_user(data_block, buf + bytes_written, current_block_bytes_to_write))
            return -EFAULT;
        bytes_to_write -= current_block_bytes_to_write;
        bytes_written += current_block_bytes_to_write;
    }

    // Step5: Update inode & osfs_inode attribute
    // extend size if needed
    osfs_inode->i_size = (*ppos + bytes_written) > osfs_inode->i_size ? (*ppos + bytes_written) : osfs_inode->i_size;
    inode->i_size = osfs_inode->i_size;
    *ppos += len;

    // Step6: Return the number of bytes written

    pr_info("osfs_write: %ld bytes written\n", bytes_written);
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
