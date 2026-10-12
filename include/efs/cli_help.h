#ifndef EFS_CLI_HELP_H
#define EFS_CLI_HELP_H
#include <stdio.h>
#include <string.h>
/* Shared help layout. Content is data; keep labels aligned and details in
 * Notes rather than crowding the synopsis. No network/storage work here. */
static inline void efs_help_section(FILE *out,const char *label,const char *text)
{
    if (!text || !*text) return;
    fprintf(out,"\n%s:\n",label);
    for (const char *p=text;*p;) {
        const char *end=strchr(p,'\n');size_t n=end ? (size_t)(end-p) : strlen(p);
        const char *split=NULL;
        if (!strcmp(label,"Options") || !strcmp(label,"Commands")) {
            for (size_t i=1;i<n;i++) if (p[i-1]==' ' && p[i]==' ') { split=p+i-1;break; }
        }
        if (split) {
            size_t width=(size_t)(split-p);const char *description=split;
            while (description<p+n && *description==' ') description++;
            if (width<=22) fprintf(out,"  %-22.*s%.*s\n",(int)width,p,(int)(p+n-description),description);
            else fprintf(out,"  %.*s\n                        %.*s\n",(int)width,p,(int)(p+n-description),description);
        } else fprintf(out,"  %.*s\n",(int)n,p);
        p=end ? end+1 : p+n;
    }
}
static inline int efs_help_requested(int argc,char **argv)
{
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--")) break;
        if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) return 1;
    }
    return 0;
}
static inline void efs_cli_help(FILE *out,const char *program,const char *topic)
{
    const char *name=strrchr(program,'/');name=name ? name+1 : program;
    const char *summary="",*syntax="",*commands=NULL,*options=NULL,*examples=NULL,*notes=NULL;
    if (!strcmp(name,"efsd")) {
        summary="Run an EFS storage node";
        syntax="--node-id ID --addr ADDRESS --port PORT --storage PATH [options]";
        options="--node-id ID          Node identifier\n--addr ADDRESS        Listen address\n--port PORT           Listen port\n--storage PATH        Storage roots; repeat or separate with commas\n--quota SIZE          Safe storage quota (default: automatic); K/M/G/T\n--storage-engine TYPE files (default) or experimental containers\n--direct-io           Use O_DIRECT for fragment I/O\n--no-direct-io        Use buffered fragment I/O (default)\n--writers N           Shared writer threads; 0 runs writes inline\n--join HOST:PORT      Join an existing cluster\n--meta-storage PATH   Raft/KV root (default: first storage root)\n--no-persist          Disable cluster configuration persistence\n--perf               Record a CPU profile\n--strace             Record system calls";
        examples="efsd --node-id 1 --addr 127.0.0.1 --port 17432 --storage /data1/efs/n1";
        notes="Metadata root changes do not move existing mdraft data.\nStorage roots use least-queue write placement across paths.\nEFS_COMMANDS_SERVER=0 disables query workers, tracking and query RPCs.\nSet the same policy on every node; changes require graceful restart.\nDefault: enabled. Re-enabling rebuilds from current persisted metadata.";
    } else if (!strcmp(name,"efs-fuse")) {
        summary="Mount an EFS export";
        syntax="NODE:PORT [NODE:PORT ...] EXPORT MOUNTPOINT [options]";
        options="-o OPTIONS           FUSE mount options\n--perf               Record a CPU profile\n--strace             Record system calls\n--stop MOUNTPOINT    Drain and stop a mounted client\n--resume MOUNTPOINT  Resume a drained client";
        examples="efs-fuse 127.0.0.1:17432 fs /data1/efs/mnt\nEFS_COMMANDS=1 efs-fuse 127.0.0.1:17432 fs /data1/efs/mnt";
        notes="One node address is enough; the client discovers the other nodes.\nEFS_COMMANDS=1 enables .efs on this mount when server policy allows it.\nThose commands require Python 3 on the machine executing them.\nUse --stop MOUNTPOINT --force-discard only to discard pending writes.\nUse --help-fuse for the underlying FUSE option reference.";
    } else if (!strcmp(name,"efs-query")) {
        summary="Query the legacy server statistics endpoint";
        syntax="[--raw] NODE:PORT";
        options="--raw                Legacy compatibility flag";
        examples="efs-query 127.0.0.1:17432";
        notes="The legacy QUERY_STATS endpoint is retired on current servers.\nUse a mounted directory's .efs/stats command for server-owned snapshots.";
    } else if (!strcmp(name,"efs-mgmt")) {
        summary="Inspect and manage an EFS cluster";syntax="COMMAND [arguments]";
        commands="status               Cluster status\nio-stats             I/O counters\ngc-status            Garbage-collection status\nversion              Node version\nadd-node             Add a node\nadd-storage          Add storage roots\nshrink-quota         Reduce a node quota\nmkfs, raft-mkfs      Format metadata\ncommands-policy      Get/set versioned command policy\nexpiry-policy        Get/set scratch expiry policy\nexpiry               Inspect/renew a file deadline\nraft-status          Raft group status\nraft-change          Change group voters\nraft-dir             Directory layout migration\nraft-session         Session lifecycle\nraft-getattr         Read inode attributes\nraft-lookup          Look up a name\nraft-lookup-path     Resolve a path\nraft-create          Create a file\nraft-unlink          Remove a file name\nraft-rmdir           Remove an empty directory\nraft-link            Create a hard link\nraft-setattr         Change attributes\nraft-rename          Rename a name\nraft-readdir         List directory entries\nraft-publish         Publish a chunk\nraft-getchunks       Read chunk mappings\nraft-append          Allocate an append range\nraft-hold            Open/close a lease\nraft-flock           Whole-file locking\nraft-fcntl           Byte-range locking";
        examples="efs-mgmt status 127.0.0.1:17432\nefs-mgmt gc-status 127.0.0.1:17432";
        notes="Run efs-mgmt COMMAND --help for its argument syntax.";
    } else if (!strcmp(name,"blake3-bench") || (topic && !strcmp(topic,"blake3"))) {
        summary="Measure BLAKE3 hashing throughput";
        syntax=!strcmp(name,"blake3-bench") ? "[options]" : "--bench blake3 [options]";
        options="--time SEC           Sample duration (default: 3)\n--threads N          Workers (default: CPUs in affinity mask)\n--size BYTES         Bytes per update (default: 65536)\n--stream             Repeated updates to one hasher (default)\n--oneshot            Initialize/finalize for each buffer";
        examples="efs-bench --bench blake3 --size 64K --threads 4 --time 3";
        notes="No storage or cluster is used. Match workers to the CPU allocation.";
    } else if (topic && (!strcmp(topic,"io") || !strcmp(topic,"io-blake3"))) {
        summary="Measure raw storage I/O";syntax="--bench io|io-blake3 --storage EMPTY_ROOT [options]";
        options="--storage PATH       Scratch root; may repeat\n--rw read|write      I/O direction\n--io-size SIZE       I/O size (default: 64K)\n--qd N               Parallel workers (default: 16)\n--window N           Blocks per worker (default: 64)\n--time SEC           Duration (default: 3)\n--direct-io          Use O_DIRECT\n--no-direct-io       Use buffered I/O\n--sync               Include fdatasync after each write\n--preallocate        Allocate and populate the window before timing";
        examples="efs-bench --bench io --storage /data1/bench --rw write --direct-io";
        notes="No EFS store or network is used. Scratch roots must be empty.\nio-blake3 hashes writes and verifies reads inside the timed loop.\nRead files are populated first; written blocks are checked afterward.\nBuffered writes without --sync measure page-cache acceptance.\nDirect I/O sizes must be aligned to 4 KiB.";
    } else if (topic && !strcmp(topic,"principles")) {
        summary="Test EFS-shaped storage principles";syntax="--bench principles --storage EMPTY_ROOT [options]";
        options="--storage PATH       Empty scratch root; may repeat\n--layout TYPE        files or container\n--rw TYPE            read, write or delete\n--policy TYPE        populated, allocating, overwrite, unlink, punch or release\n--persist TYPE       none, each or batch\n--objects N          Object count\n--payload-size SIZE  Payload bytes\n--qd N               Parallel workers\n--batch-size N       Objects per durability batch\n--container-alignment N Packed (1) or block-aligned (4096) records\n--directory-sync TYPE worker or ancestors\n--logical-size SIZE  Logical content bytes inside the payload\n--indexed            Experimental immutable segments and manifest\n--retain             Keep a durable write fixture\n--recover-existing   Verify a retained fixture read-only\n--direct-io          Use O_DIRECT\n--no-direct-io       Use buffered I/O";
        examples="efs-bench --bench principles --storage /data1/bench --layout files";
        notes="Independent principle driver; no production engine or cluster.\nFixtures make one finite pass, rather than --time loops.\nPayloads have a 32-byte checksum tail; direct records use a 4 KiB tail.\neach/batch includes required file and directory fsync in wall time.";
    } else if (topic && (!strcmp(topic,"data") || !strcmp(topic,"meta"))) {
        summary="Measure local EFS storage engines";syntax="--bench data|meta --storage EMPTY_ROOT [options]";
        options="--storage PATH       Data scratch roots; repeat or comma-separate\n--meta-storage PATH  Metadata scratch root\n--time SEC           Duration (default: 10)\n--writers N          Writer threads\n--qd N               Queue depth\n--window N           Bounded fragments per slot\n--rw TYPE            read, write or both\n--write-mode TYPE    create or overwrite\n--direct-io          Use O_DIRECT\n--no-direct-io       Use buffered I/O\n--sync               Use O_SYNC for engine writes\n--full-paths         Skip the path-prefix ladder\n--writer-stats       Measure writer waits\n--skip-ceiling       Skip the separate raw/fio ceiling probe\n--perf               Record a CPU profile";
        examples="efs-bench --bench data --storage /data1/bench --time 10\nefs-bench --bench meta --meta-storage /data1/meta-bench --time 10";
        notes="Local engines: no cluster or sockets. Scratch roots must be empty.\nRead/overwrite requires --window; create requires no window.\nBounded windows are populated before timing.\nPercentile buckets have less than 0.782% width; maximum is exact.";
    } else {
        summary="Measure EFS network, storage and metadata performance";syntax="[NODE:PORT] MODE [options]";
        commands="--time SEC           Network ceiling; servers ACK and discard data\n--size SIZE          Write real protected data (K/M/G/T suffixes)\n--store --time SEC   Timed disk writes\n--read --time SEC    Timed disk reads\n--meta               Metadata RPC workload\n--bench io           Raw storage I/O (no cluster)\n--bench io-blake3    Raw I/O with timed hashing\n--bench blake3       CPU hashing (no storage or cluster)\n--bench data         Local data engine (no cluster)\n--bench meta         Local metadata engine (no cluster)\n--bench ranges       Shared-container contention experiments\n--bench churn        Bounded mixed storage experiments\n--bench principles   Independent EFS-shaped storage experiments";
        options="--window N           Read window in chunks\n--export NAME        Metadata export\n--files N            Metadata file count\n--dirs N             Metadata directory count\n--workers N          Metadata workers\n--phases CSV         Metadata phases\n--keep               Keep metadata fixture\n--id N               Chunk-index base for parallel writers\n--perf               Record a CPU profile";
        examples="efs-bench 127.0.0.1:17432 --store --time 10\nefs-bench --bench data --help\nefs-bench --bench principles --help";
        notes="Run efs-bench --bench MODE --help for local mode options.\nCluster modes discover all servers from one node address.";
    }
    fprintf(out,"%s - %s\n",name,summary);
    fprintf(out,"\nUsage:\n  %s %s\n",name,syntax);
    efs_help_section(out,"Commands",commands);
    if (options && *options) {
        efs_help_section(out,"Options",options);
        fprintf(out,"  %-22sShow this help\n","-h, --help");
        if (strcmp(name,"blake3-bench")) fprintf(out,"  %-22sShow the program version\n","--version");
    } else efs_help_section(out,"Options",!strcmp(name,"blake3-bench") ? "-h, --help           Show this help" :
        "-h, --help           Show this help\n--version            Show the program version");
    efs_help_section(out,"Examples",examples);efs_help_section(out,"Notes",notes);
}
/* Argument reference for administrative subcommands. Kept separate from the
 * overview so operational help stays compact and never opens a connection. */
static inline int efs_mgmt_help(FILE *out,const char *command)
{
    static const struct { const char *name,*args; } specs[]={
        {"status","NODE:PORT"},{"io-stats","NODE:PORT"},{"gc-status","NODE:PORT"},
        {"version","NODE:PORT"},{"add-node","NEW_NODE:PORT EXISTING_NODE:PORT"},
        {"shrink-quota","NODE:PORT AMOUNT[K|M|G|T]"},{"add-storage","NODE:PORT PATH[,PATH...]"},
        {"expiry-policy","NODE:PORT get ROOT GENERATION | set ROOT GENERATION EXPECTED_REV TTL_NS MAX_NS FLAGS"},
        {"expiry","NODE:PORT get INO GENERATION | renew INO GENERATION EXPECTED_REV DEADLINE_NS"},
        {"commands-policy","NODE:PORT get | set MASK EXPECTED_VERSION"},{"mkfs","NODE:PORT [legacy-label]"},{"raft-mkfs","NODE:PORT [legacy-label]"},
        {"raft-status","NODE:PORT"},{"raft-change","NODE:PORT GROUP VOTERS"},
        {"raft-dir","NODE:PORT INO begin|migrate|finish"},
        {"raft-session","NODE:PORT create|register|establish|get|fence|cleanup-leases|cleanup-appends UUID_HEX [epoch|shard] [shard]"},
        {"raft-getattr","NODE:PORT [ino]"},{"raft-lookup","NODE:PORT PARENT NAME"},
        {"raft-create","NODE:PORT PARENT NAME [mode]"},{"raft-unlink","NODE:PORT PARENT NAME"},
        {"raft-rmdir","NODE:PORT PARENT NAME"},{"raft-link","NODE:PORT SRC_INO PARENT NAME"},
        {"raft-setattr","NODE:PORT INO MASK [mode|mtime|size] [uid|atime] [gid]"},
        {"raft-rename","NODE:PORT OLD_PARENT OLD_NAME NEW_PARENT NEW_NAME"},
        {"raft-readdir","NODE:PORT PARENT [after_ino]"},{"raft-lookup-path","NODE:PORT PATH [start]"},
        {"raft-publish","NODE:PORT INO [chunk] [size]"},{"raft-getchunks","NODE:PORT INO [start]"},
        {"raft-append","NODE:PORT INO LENGTH [uuid-hex epoch]"},
        {"raft-hold","NODE:PORT INO open|close [owner] [uuid-hex epoch]"},
        {"raft-flock","NODE:PORT INO ex|sh|un|gex|gsh|wex|wsh [owner] [uuid-hex epoch]"},
        {"raft-fcntl","NODE:PORT INO ex|sh|un|gex|gsh|wex|wsh [owner] [start end] [uuid-hex epoch]"}
    };
    if (!command || !strcmp(command,"--help") || !strcmp(command,"-h")) {
        efs_cli_help(out,"efs-mgmt",NULL);return 0;
    }
    for (size_t i=0;i<sizeof(specs)/sizeof(specs[0]);i++) if (!strcmp(command,specs[i].name)) {
        fprintf(out,"efs-mgmt %s - Administrative command\n\nUsage:\n  efs-mgmt %s %s\n",
                command,command,specs[i].args);
        efs_help_section(out,"Options","-h, --help           Show this help");
        efs_help_section(out,"Notes",(!strcmp(command,"mkfs") || !strcmp(command,"raft-mkfs")) ?
            "Single export; the legacy label is accepted and ignored.\nNode addresses use HOST:PORT." :
            "Node addresses use HOST:PORT.");return 0;
    }
    fprintf(stderr,"Unknown command: %s\n",command);return 1;
}
#endif
