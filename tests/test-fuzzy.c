#include <fuzzy.h>
#include <ide-line-reader.h>
#include <stdlib.h>
#include <string.h>

int
main (int argc,
      char *argv[])
{
  IdeLineReader reader;
  const gchar *param;
  Fuzzy *fuzzy;
  GArray *ar;
  gchar *contents;
  gchar *line;
  gsize len;
  gsize line_len;

  if (argc < 3)
    {
      g_printerr ("usage: %s FILENAME QUERY\n", argv[0]);
      return 1;
    }

  fuzzy = fuzzy_new (FALSE);

  g_print ("Loading contents\n");
  g_file_get_contents (argv [1], &contents, &len, NULL);
  g_print ("Loaded\n");

  ide_line_reader_init (&reader, contents, len);

  fuzzy_begin_bulk_insert (fuzzy);

  g_print ("Building index.\n");
  while ((line = ide_line_reader_next (&reader, &line_len)))
    {
      line [line_len] = '\0';
      fuzzy_insert (fuzzy, line, NULL);
    }
  fuzzy_end_bulk_insert (fuzzy);
  g_print ("Built.\n");

  g_free (contents);

  if (!g_utf8_validate (argv[2], -1, NULL))
    {
      g_critical ("Invalid UTF-8 discovered, aborting.");
      return EXIT_FAILURE;
    }

  if (strlen (argv[2]) > 256)
    {
      g_critical ("Only supports searching of up to 256 characters.");
      return EXIT_FAILURE;
    }

  param = (const gchar *)argv[2];

  ar = fuzzy_match (fuzzy, param, 0);

  for (guint i = 0; i < ar->len; i++)
    {
      FuzzyMatch *m = &g_array_index (ar, FuzzyMatch, i);

      g_print ("%0.3lf: %s\n", m->score, m->key);
    }

  g_print ("%d matches\n", ar->len);

  g_array_unref (ar);

  fuzzy_unref (fuzzy);

  return 0;
}
