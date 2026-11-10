package com.rinoize.rimu2000;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Copies only recognized ROMs into private storage; never replaces a partial file. */
final class RomImporter {
    private static final Map<String, Spec> SPECS = new LinkedHashMap<>();
    static {
        add("mu2000_flash.bin", "mu2000_flash.bin", 0x400000, true);
        add("xv364a0.ic49", "dump/xv364a0.ic49", 0x800000, true);
        add("xv365a0.ic50", "dump/xv365a0.ic50", 0x800000, true);
        add("xw848a0.ic53", "dump/xw848a0.ic53", 0x800000, true);
        add("xw849a0.ic54", "dump/xw849a0.ic54", 0x800000, true);
        add("sin-table.bin", "standin/sin-table.bin", 0x10000, false);
        add("hd44780u_b04.bin", "hd44780u_b04.bin", 0x1000, false);
    }

    private static final class Spec {
        final String name, relative;
        final int bytes;
        final boolean required;
        Spec(String name, String relative, int bytes, boolean required) {
            this.name = name; this.relative = relative; this.bytes = bytes;
            this.required = required;
        }
    }

    private static void add(String name, String path, int bytes, boolean required) {
        SPECS.put(name, new Spec(name, path, bytes, required));
    }

    static List<String> missing(File root) {
        List<String> out = new ArrayList<>();
        for (Spec spec : SPECS.values()) {
            if (!spec.required) continue;
            File file = new File(root, spec.relative);
            if (!file.isFile() || file.length() != spec.bytes) out.add(spec.name);
        }
        return out;
    }

    static String summary(File root) {
        List<String> missing = missing(root);
        if (missing.isEmpty()) return "ROMs listas · firmware + 4 ROMs de ondas (36 MiB)";
        return (5 - missing.size()) + "/5 ROMs necesarias · faltan: "
                + String.join(", ", missing);
    }

    static String importFiles(ContentResolver resolver, List<Uri> files, File root) {
        int copied = 0;
        List<String> errors = new ArrayList<>();
        for (Uri uri : files) {
            try {
                String name = displayName(resolver, uri);
                Spec spec = SPECS.get(name.toLowerCase(Locale.ROOT));
                if (spec == null) {
                    errors.add(name + ": nombre no reconocido");
                    continue;
                }
                copy(resolver, uri, new File(root, spec.relative), spec.bytes);
                copied++;
            } catch (IOException | RuntimeException e) {
                errors.add(readable(e));
            }
        }
        return result(copied, errors, root);
    }

    static String importTree(ContentResolver resolver, Uri tree, File root) {
        List<Uri> files = new ArrayList<>();
        try {
            int[] visited = {0};
            collect(resolver, tree, DocumentsContract.getTreeDocumentId(tree),
                    0, visited, files);
            if (files.isEmpty()) return "No encontré ROMs reconocidas en esa carpeta.\n"
                    + summary(root);
            return importFiles(resolver, files, root);
        } catch (IOException | RuntimeException e) {
            return "No se pudo leer la carpeta: " + readable(e);
        }
    }

    private static void collect(ContentResolver resolver, Uri tree, String documentId,
                                int depth, int[] visited, List<Uri> files) throws IOException {
        if (depth > 5) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE};
        List<String> directories = new ArrayList<>();
        try (Cursor cursor = resolver.query(children, columns, null, null, null)) {
            if (cursor == null) throw new IOException("El proveedor no respondió.");
            while (cursor.moveToNext()) {
                if (++visited[0] > 2000) throw new IOException(
                        "La carpeta contiene demasiados archivos; elegí la carpeta de ROMs.");
                String id = cursor.getString(0), name = cursor.getString(1);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) {
                    directories.add(id);
                } else if (name != null && SPECS.containsKey(name.toLowerCase(Locale.ROOT))) {
                    files.add(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                }
            }
        }
        for (String directory : directories) {
            collect(resolver, tree, directory, depth + 1, visited, files);
        }
    }

    private static String displayName(ContentResolver resolver, Uri uri) throws IOException {
        try (Cursor cursor = resolver.query(uri, new String[]{OpenableColumns.DISPLAY_NAME},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                String name = cursor.getString(0);
                if (!name.contains("/") && !name.contains("\\")) return name;
            }
        }
        throw new IOException("No se pudo identificar el nombre del archivo.");
    }

    private static void copy(ContentResolver resolver, Uri source, File target, int bytes)
            throws IOException {
        File parent = target.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) {
            throw new IOException("No se pudo crear la carpeta de ROMs.");
        }
        File staging = File.createTempFile("rom-", ".part", parent);
        try {
            try (InputStream input = resolver.openInputStream(source);
                 FileOutputStream output = new FileOutputStream(staging)) {
                if (input == null) throw new IOException("No se pudo abrir " + target.getName());
                byte[] buffer = new byte[65536];
                int total = 0, n;
                while ((n = input.read(buffer)) != -1) {
                    if (n == 0) continue;
                    if (n > bytes - total) throw new IOException(
                            target.getName() + ": tamaño mayor al esperado (" + bytes + " bytes)");
                    output.write(buffer, 0, n);
                    total += n;
                }
                if (total != bytes) throw new IOException(target.getName()
                        + ": esperaba " + bytes + " bytes; recibí " + total);
                output.getFD().sync();
            }
            // POSIX rename on the same filesystem replaces atomically. The old ROM
            // is untouched until the new file has the exact size and has been synced.
            try {
                Os.rename(staging.getAbsolutePath(), target.getAbsolutePath());
            } catch (ErrnoException e) {
                throw new IOException("No se pudo guardar " + target.getName(), e);
            }
        } finally {
            if (staging.exists()) staging.delete();
        }
    }

    private static String result(int copied, List<String> errors, File root) {
        StringBuilder text = new StringBuilder("Archivos importados: ").append(copied)
                .append(".\n").append(summary(root));
        for (int i = 0; i < Math.min(errors.size(), 7); i++) text.append("\n• ").append(errors.get(i));
        if (errors.size() > 7) text.append("\nY ").append(errors.size() - 7).append(" archivos rechazados más.");
        return text.toString();
    }

    private static String readable(Throwable error) {
        return error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage();
    }
}
